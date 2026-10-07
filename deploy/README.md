# AWS EC2 Deployment Guide

Step-by-step instructions to deploy the Water Quality Monitor server on AWS EC2 (Amazon Linux 2023 or Ubuntu).

---

## Prerequisites

- An AWS account
- Basic familiarity with the AWS Console and SSH
- GitHub repository secrets configured:
  - `EC2_HOST`
  - `EC2_USER`
  - `EC2_PORT`
  - `EC2_PPK_KEY` (full PuTTY `.ppk` key text)

---

## 1. Launch an EC2 Instance

1. Go to **AWS Console → EC2 → Launch Instance**
2. Configure:
   - **Name**: `WaterQualityMonitor`
   - **AMI**: 
     - **Amazon Linux 2023** (Recommended) or **Amazon Linux 2**
     - *or* **Ubuntu Server 22.04 LTS**
   - **Instance type**: `t2.micro` (free tier eligible) or `t3.micro`
   - **Key pair**: Create or select an existing key pair (for SSH access)
   - **Security Group**: Create a new one with these inbound rules:

| Type       | Port | Source    | Purpose          |
|------------|------|-----------|------------------|
| SSH        | 22   | Your IP   | Remote access    |
| Custom TCP | 1883 | 0.0.0.0/0 | MQTT (ESP32 4G) |
| Custom TCP | 8000 | 0.0.0.0/0 | Dashboard        |

3. Click **Launch Instance**

> [!IMPORTANT]
> Your EC2 security group must allow inbound:
> - **SSH (TCP 22)** for deployment access
> - **Dashboard (TCP 8000)** for web access

## 2. Assign an Elastic IP

1. Go to **EC2 → Elastic IPs → Allocate Elastic IP**
2. Select the new IP → **Actions → Associate** → choose your instance
3. **Note this IP** — you'll need it for `config.h` (`MQTT_BROKER`)

## 3. Connect via SSH

Replace `<DEFAULT-USER>` with:
- `ec2-user` for **Amazon Linux**
- `ubuntu` for **Ubuntu**

```bash
chmod 400 your-key.pem

# For Amazon Linux:
ssh -i your-key.pem ec2-user@<ELASTIC-IP>

# For Ubuntu:
ssh -i your-key.pem ubuntu@<ELASTIC-IP>
```

## 4. Upload Project Files

From your local machine:

```bash
# For Amazon Linux:
scp -i your-key.pem -r ./server ec2-user@<ELASTIC-IP>:~/
scp -i your-key.pem -r ./deploy ec2-user@<ELASTIC-IP>:~/

# For Ubuntu:
scp -i your-key.pem -r ./server ubuntu@<ELASTIC-IP>:~/
scp -i your-key.pem -r ./deploy ubuntu@<ELASTIC-IP>:~/
```

## 5. Run the Setup Script

On the EC2 instance:
```bash
cd ~/deploy
chmod +x setup_ec2.sh
sudo ./setup_ec2.sh
```

The script auto-detects Amazon Linux vs Ubuntu and configures:
- Package manager (`dnf` / `apt`)
- Mosquitto MQTT broker
- Python virtual environment & dependencies
- `waterquality` systemd service

## 6. Verify

### Check services are running:
```bash
sudo systemctl status mosquitto
sudo systemctl status waterquality
```

### Test MQTT (from EC2):
```bash
# In one terminal — subscribe:
mosquitto_sub -h localhost -t "waterquality/#" -v

# In another terminal — publish test message:
mosquitto_pub -h localhost -t "waterquality/WQM-001/sensors/live" \
  -m '{"device_id":"WQM-001","sensors":{"ph":{"value":7.2,"unit":"pH"},"tds":{"value":150,"unit":"ppm"},"temperature":{"value":28.0,"unit":"°C"},"turbidity":{"value":2100,"unit":"NTU"},"dissolved_oxygen":{"value":6.5,"unit":"mg/L"}},"metadata":{"signal_quality":18,"firmware_version":"2.0.0","uptime_seconds":60}}'
```

### Open dashboard:
Navigate to `http://<ELASTIC-IP>:8000` in your browser.  
Default credentials: **admin** / **waterquality**

## 7. Update ESP32 Config

In `firmware/config.h`, set:
```cpp
#define MQTT_BROKER  "<YOUR-ELASTIC-IP>"
```

Flash the firmware and power on — data should appear in the dashboard.

---

## Manual Deployment from GitHub Actions (Recommended)

This repository includes a manual-only workflow:

- **Workflow file:** `.github/workflows/deploy-ec2-manual.yml`
- **Trigger:** `workflow_dispatch` only (runs only when manually approved/run)

### Run steps
1. Open **GitHub → Actions → Manual EC2 Deploy**.
2. Click **Run workflow**.
3. Select the branch and confirm the run.

### What it does
- Uses a GitHub-hosted Ubuntu runner.
- Installs `putty-tools` and converts `EC2_PPK_KEY` to a temporary OpenSSH key in runner temp storage.
- Uses strict SSH host-key checking with a generated `known_hosts` entry.
- Securely copies `server/` and `deploy/` to the EC2 host over SSH.
- Runs `deploy/setup_ec2.sh`, then restarts and verifies the `waterquality` service.

> The workflow does not hardcode host/user/port/key values and does not print secret values.

---

## Maintenance

| Command | Purpose |
|---------|---------|
| `sudo systemctl restart waterquality` | Restart the server |
| `sudo journalctl -u waterquality -f` | View server logs |
| `sudo systemctl restart mosquitto` | Restart MQTT broker |
| `mosquitto_sub -h localhost -t "waterquality/#" -v` | Monitor all MQTT messages |

---

## Enable MQTT Password Auth (Production)

```bash
# Create password file
sudo mosquitto_passwd -c /etc/mosquitto/passwd wqm_device

# Edit config
sudo nano /etc/mosquitto/conf.d/waterquality.conf
# Change: allow_anonymous false
# Add:    password_file /etc/mosquitto/passwd

# Restart
sudo systemctl restart mosquitto
```

Then update `config.h`:
```cpp
#define MQTT_USERNAME  "wqm_device"
#define MQTT_PASSWORD  "your_password_here"
```
