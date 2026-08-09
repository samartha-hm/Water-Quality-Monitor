#!/bin/bash
# =============================================================
# setup_ec2.sh — EC2 Instance Bootstrap Script
# Water Quality Monitor — AWS Deployment
# =============================================================
# Supports: Amazon Linux 2023 / Amazon Linux 2 / Ubuntu 22.04+
#
# Run on a fresh EC2 instance:
#   chmod +x setup_ec2.sh && sudo ./setup_ec2.sh
# =============================================================

set -e

echo "============================================="
echo " Water Quality Monitor — EC2 Setup"
echo "============================================="

# --- Detect OS ---
if [ -f /etc/os-release ]; then
    . /etc/os-release
    OS_ID="$ID"
else
    OS_ID="unknown"
fi

echo "[0/7] Detected OS: $OS_ID ($PRETTY_NAME)"

# Set package manager and default user based on OS
if [[ "$OS_ID" == "amzn" ]]; then
    PKG_INSTALL="dnf install -y"
    PKG_UPDATE="dnf update -y"
    SVC_USER="ec2-user"
    echo "  → Using dnf (Amazon Linux)"
elif [[ "$OS_ID" == "ubuntu" || "$OS_ID" == "debian" ]]; then
    PKG_INSTALL="apt-get install -y"
    PKG_UPDATE="apt-get update -y && apt-get upgrade -y"
    SVC_USER="ubuntu"
    echo "  → Using apt (Ubuntu/Debian)"
else
    echo "  ⚠ Unsupported OS: $OS_ID. Trying dnf..."
    PKG_INSTALL="dnf install -y"
    PKG_UPDATE="dnf update -y"
    SVC_USER="ec2-user"
fi

# --- 1. System Update ---
echo "[1/7] Updating system packages..."
eval $PKG_UPDATE

# --- 2. Install Mosquitto MQTT Broker ---
echo "[2/7] Installing Mosquitto MQTT broker..."

if [[ "$OS_ID" == "amzn" ]]; then
    echo "  → Installing Mosquitto on Amazon Linux..."
    if ! dnf install -y mosquitto mosquitto-clients 2>/dev/null; then
        echo "  → Package not in default repo. Building Mosquitto from source for AL2023..."
        dnf install -y gcc gcc-c++ make openssl-devel libuuid-devel wget >/dev/null 2>&1 || true

        if ! command -v mosquitto &> /dev/null; then
            BUILD_DIR="/tmp/mosquitto-build"
            mkdir -p $BUILD_DIR
            cd $BUILD_DIR
            wget -q https://mosquitto.org/files/source/mosquitto-2.0.18.tar.gz
            tar -xzf mosquitto-2.0.18.tar.gz
            cd mosquitto-2.0.18
            make WITH_CJSON=no WITH_CONTROL=no WITH_SYSTEMD=no > /dev/null
            make install > /dev/null
            ldconfig 2>/dev/null || true
            useradd -r -s /bin/false mosquitto 2>/dev/null || true
            cd /opt/waterquality 2>/dev/null || cd ~/
            echo "  → Mosquitto compiled and installed successfully!"
        fi
    fi
else
    apt-get install -y mosquitto mosquitto-clients
fi

# Copy custom config if present
if [ -f ./mosquitto.conf ]; then
    # Ensure conf.d directory exists
    mkdir -p /etc/mosquitto/conf.d
    cp ./mosquitto.conf /etc/mosquitto/conf.d/waterquality.conf
    echo "  → Custom config copied to /etc/mosquitto/conf.d/"
fi

# Ensure systemd service file exists for Mosquitto
if [ ! -f /lib/systemd/system/mosquitto.service ] && [ ! -f /etc/systemd/system/mosquitto.service ]; then
    cat > /etc/systemd/system/mosquitto.service << 'EOF'
[Unit]
Description=Mosquitto MQTT Broker
After=network.target

[Service]
Type=simple
ExecStart=/usr/local/sbin/mosquitto -c /etc/mosquitto/conf.d/waterquality.conf
Restart=always

[Install]
WantedBy=multi-user.target
EOF
    systemctl daemon-reload
fi

# Enable and start Mosquitto
systemctl enable mosquitto
systemctl restart mosquitto
echo "  → Mosquitto running on port 1883"

# --- 3. Install Python 3 ---
echo "[3/7] Installing Python 3 and pip..."

if [[ "$OS_ID" == "amzn" ]]; then
    dnf install -y python3 python3-pip python3-devel
else
    apt-get install -y python3 python3-pip python3-venv
fi

# --- 4. Create Application Directory ---
echo "[4/7] Setting up application directory..."
APP_DIR="/opt/waterquality"
mkdir -p $APP_DIR
mkdir -p $APP_DIR/logs

# Copy server files (assumes they are in the current directory)
if [ -d ../server ]; then
    cp -r ../server/* $APP_DIR/
    echo "  → Server files copied from ../server/ to $APP_DIR"
elif [ -d ./server ]; then
    cp -r ./server/* $APP_DIR/
    echo "  → Server files copied from ./server/ to $APP_DIR"
else
    echo "  ⚠ No server/ directory found. Copy files manually to $APP_DIR"
fi

# Set ownership
chown -R $SVC_USER:$SVC_USER $APP_DIR

# --- 5. Python Virtual Environment & Dependencies ---
echo "[5/7] Creating Python virtual environment..."
python3 -m venv $APP_DIR/venv
source $APP_DIR/venv/bin/activate
pip install --upgrade pip
if [ -f $APP_DIR/requirements.txt ]; then
    pip install -r $APP_DIR/requirements.txt
    echo "  → Python dependencies installed"
else
    echo "  ⚠ requirements.txt not found in $APP_DIR"
fi
deactivate

# --- 6. Create Systemd Service ---
echo "[6/7] Creating systemd service..."
cat > /etc/systemd/system/waterquality.service << EOF
[Unit]
Description=Water Quality Monitor Server
After=network.target mosquitto.service
Requires=mosquitto.service

[Service]
Type=simple
User=$SVC_USER
WorkingDirectory=/opt/waterquality
ExecStart=/opt/waterquality/venv/bin/python -m uvicorn server:app --host 0.0.0.0 --port 8000
Restart=always
RestartSec=5
Environment=MQTT_BROKER=localhost
Environment=MQTT_PORT=1883
Environment=DASH_USERNAME=admin
Environment=DASH_PASSWORD=waterquality

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable waterquality
systemctl start waterquality
echo "  → Service 'waterquality' created and started (user: $SVC_USER)"

# --- 7. Configure Firewall ---
echo "[7/7] Configuring firewall..."

if [[ "$OS_ID" == "amzn" ]]; then
    # Amazon Linux uses iptables or firewalld (security groups are primary firewall)
    if command -v firewall-cmd &> /dev/null; then
        firewall-cmd --permanent --add-port=1883/tcp
        firewall-cmd --permanent --add-port=8000/tcp
        firewall-cmd --reload
        echo "  → firewalld: opened ports 1883, 8000"
    else
        echo "  → No local firewall detected. Using EC2 Security Groups only."
        echo "  → Make sure your Security Group allows inbound TCP 1883 and 8000!"
    fi
else
    apt-get install -y ufw
    ufw allow 22/tcp      # SSH
    ufw allow 1883/tcp    # MQTT
    ufw allow 8000/tcp    # Dashboard
    ufw --force enable
    echo "  → UFW configured: SSH(22), MQTT(1883), Dashboard(8000)"
fi

echo ""
echo "============================================="
echo " ✅ Setup Complete!"
echo "============================================="
echo ""
echo " OS:          $PRETTY_NAME"
echo " User:        $SVC_USER"
echo " Mosquitto:   Port 1883 (open)"
echo " Dashboard:   http://$(curl -s ifconfig.me 2>/dev/null || echo '<YOUR-IP>'):8000"
echo " Auth:        admin / waterquality"
echo ""
echo " Useful commands:"
echo "   sudo systemctl status waterquality"
echo "   sudo systemctl restart waterquality"
echo "   sudo journalctl -u waterquality -f"
echo "   mosquitto_sub -h localhost -t 'waterquality/#' -v"
echo ""
echo " To change dashboard password:"
echo "   sudo systemctl edit waterquality"
echo "   Add: Environment=DASH_PASSWORD=your_new_password"
echo "   sudo systemctl restart waterquality"
echo ""
echo " To set MQTT password authentication:"
echo "   sudo mosquitto_passwd -c /etc/mosquitto/passwd wqm_device"
echo "   Edit /etc/mosquitto/conf.d/waterquality.conf:"
echo "     allow_anonymous false"
echo "     password_file /etc/mosquitto/passwd"
echo "   sudo systemctl restart mosquitto"
echo ""
echo "============================================="
