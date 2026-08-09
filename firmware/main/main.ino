#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>

#define DHTPIN 8        // DHT11 connected to Digital Pin 7
#define DHTTYPE DHT11

DHT dht(DHTPIN, DHTTYPE);

LiquidCrystal_I2C lcd(0x27, 16, 2);

void setup()
{
  Serial.begin(9600);

  dht.begin();

  lcd.init();
  lcd.backlight();

  lcd.setCursor(0,0);
  lcd.print("TEMP:");

  lcd.setCursor(0,1);
  lcd.print("HUMI:");
}

void loop()
{
  delay(2000);

  float temperature = dht.readTemperature();
  float humidity = dht.readHumidity();

  if (isnan(temperature) || isnan(humidity))
  {
    Serial.println("DHT11 Error");
    return;
  }

  Serial.print("Temperature: ");
  Serial.print(temperature);
  Serial.println(" C");

  Serial.print("Humidity: ");
  Serial.print(humidity);
  Serial.println(" %");

  lcd.setCursor(6,0);
  lcd.print("      ");
  lcd.setCursor(6,0);
  lcd.print(temperature);
  lcd.print((char)223);   // Degree symbol
  lcd.print("C");

  lcd.setCursor(6,1);
  lcd.print("      ");
  lcd.setCursor(6,1);
  lcd.print(humidity);
  lcd.print("%");
}