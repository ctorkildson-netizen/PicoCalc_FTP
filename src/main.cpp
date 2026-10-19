/* PicoCalc FTP server
 *
 *  - Esc returns to the PicoCalc boot menu (UF2 Loader + PicoCalc Shell),
 *    so no power cycle is needed
 *  - settings come from /config_ftp.conf; if it's missing, the WiFi
 *    settings (and time zone) come from /settime.cfg or
 *    /virtualt/virtualt.cfg, and the FTP login defaults to
 *    picocalc / picocalc
 *  - config lines may have spaces around '=' and '#' comments; WiFi
 *    passwords up to 79 characters
 *  - the clock is set from NTP (or kept from an earlier program in the
 *    RP2350's always-on timer), so uploaded files get real dates
 *  - uploads go to "name.~up" and replace the old file only once they
 *    have fully arrived; a failed upload leaves the old file as it was
 *  - every command the client sends is shown on screen, with the result
 *    of each upload
 */
#include <WiFi.h>
#include <SPI.h>
#include <Wire.h>
#include <time.h>
#include <sys/time.h>
#include <LovyanGFX.hpp>
#include "LGFX_config.hpp"
#include <SdFat.h>
#include <sdios.h>
#include <SimpleFTPServer.h>
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "pico/aon_timer.h"

#define SS_PIN 17
#define SD_DET_PIN 22

/* keyboard (the STM32 "south bridge") on I2C1 */
#define SB_SDA  6
#define SB_SCL  7
#define SB_ADDR 0x1F
#define SB_REG_FIF 0x09
#define KEY_ESC 0xB1

static LGFX lcd;
FtpServer ftpSrv;
SdFat sd;

static char ssid[64], password[80], ftp_user[32] = "picocalc", ftp_password[32] = "picocalc";
static char tz[64] = "PST8PDT,M3.2.0,M11.1.0";
static const char *cfg_used = 0;

/* ------------------------------------------------------------ settings */

static char *trim(char *s)
{
  while (*s == ' ' || *s == '\t') s++;
  char *e = s + strlen(s);
  while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
  return s;
}

static void set(char *dst, size_t n, const char *v) { strncpy(dst, v, n - 1); dst[n - 1] = 0; }

/* read key=value lines; returns true if the file was there */
static bool read_cfg(const char *path, bool ftp_keys)
{
  FsFile f = sd.open(path, O_RDONLY);
  if (!f) return false;
  char line[160];
  while (f.available())
  {
    int len = f.readBytesUntil('\n', line, sizeof line - 1);
    line[len] = 0;
    char *l = trim(line);
    if (!*l || *l == '#') continue;
    char *eq = strchr(l, '=');
    if (!eq) continue;
    *eq = 0;
    char *k = trim(l), *v = trim(eq + 1);
    if (!strcasecmp(k, "ssid"))          set(ssid, sizeof ssid, v);
    else if (!strcasecmp(k, "password")) set(password, sizeof password, v);
    else if (!strcasecmp(k, "tz"))       set(tz, sizeof tz, v);
    else if (ftp_keys && !strcasecmp(k, "ftp_user"))     set(ftp_user, sizeof ftp_user, v);
    else if (ftp_keys && !strcasecmp(k, "ftp_password")) set(ftp_password, sizeof ftp_password, v);
  }
  f.close();
  return true;
}

static void load_settings()
{
  if (read_cfg("/config_ftp.conf", true))
  {
    cfg_used = "config_ftp.conf";
    if (ssid[0]) return;
  }
  if (read_cfg("/settime.cfg", false) && ssid[0])
    cfg_used = cfg_used ? "config_ftp.conf + settime.cfg" : "settime.cfg";
  else if (read_cfg("/virtualt/virtualt.cfg", false) && ssid[0])
    cfg_used = cfg_used ? "config_ftp.conf + virtualt.cfg" : "virtualt/virtualt.cfg";
}

/* ------------------------------------------------------------ keyboard */

static bool esc_pressed()
{
  Wire1.beginTransmission(SB_ADDR);
  Wire1.write(SB_REG_FIF);
  if (Wire1.endTransmission() != 0) return false;
  if (Wire1.requestFrom(SB_ADDR, 2) != 2) return false;
  uint8_t state = Wire1.read(), key = Wire1.read();
  return state == 1 && key == KEY_ESC;      /* 1 = pressed */
}

/* back to the PicoCalc Shell: the same request picocalc_shell.h makes
   (UF2 Loader 2.3+, stage3 loads /BOOT2350.UF2) */
static void return_to_menu()
{
  lcd.println("\nBack to the menu...");
  sd.end();
  delay(200);
  pinMode(SD_DET_PIN, INPUT_PULLUP);
  delayMicroseconds(100);
  if (digitalRead(SD_DET_PIN))              /* no card: the loader couldn't start the shell */
  {
    lcd.println("No SD card -- switch off instead.");
    for (;;) delay(1000);
  }
  watchdog_hw->scratch[1] = 1;              /* boot from the card */
  watchdog_hw->scratch[2] = 0;
  watchdog_hw->scratch[0] = 0xe98cc638u;
  watchdog_reboot(0, 0, 0);
  for (;;) ;
}

static void wait_or_menu(const char *why)
{
  lcd.println(why);
  lcd.println("Press Esc for the menu.");
  for (;;) { if (esc_pressed()) return_to_menu(); delay(50); }
}

/* ------------------------------------------------------------ clock */

/* FAT file dates, in local time */
static void fat_datetime(uint16_t *date, uint16_t *time_)
{
  time_t now = time(NULL);
  struct tm t;
  localtime_r(&now, &t);
  *date = FS_DATE(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  *time_ = FS_TIME(t.tm_hour, t.tm_min, t.tm_sec);
}

static bool clock_is_set() { return time(NULL) > 1600000000; }

static void set_clock()
{
  setenv("TZ", tz, 1);
  tzset();

  /* the always-on timer keeps the time from an earlier program
     (settime, VirtualT) until the power goes off */
  struct timespec ts;
  if (aon_timer_is_running() && aon_timer_get_time(&ts) && ts.tv_sec > 1600000000)
  {
    struct timeval tv = { ts.tv_sec, (suseconds_t)(ts.tv_nsec / 1000) };
    settimeofday(&tv, NULL);
  }
  else
  {
    lcd.print("Clock: asking pool.ntp.org...");
    NTP.begin("pool.ntp.org");
    uint32_t until = millis() + 10000;
    while (!clock_is_set() && millis() < until) delay(100);
    if (clock_is_set())
    {
      struct timeval tv;
      gettimeofday(&tv, NULL);
      struct timespec t2 = { tv.tv_sec, tv.tv_usec * 1000 };
      if (aon_timer_is_running()) aon_timer_set_time(&t2); else aon_timer_start(&t2);
    }
    lcd.println(clock_is_set() ? " ok" : " no answer");
  }
  if (clock_is_set())
  {
    FsDateTime::setCallback(fat_datetime);
    time_t now = time(NULL);
    char buf[40];
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", localtime(&now));
    lcd.print("Time: ");
    lcd.println(buf);
  }
  else
    lcd.println("Files will be undated.");
}

/* ------------------------------------------------------------ log */

static void ftp_log(const char *line)
{
  char t[12] = "";
  if (clock_is_set())
  {
    time_t now = time(NULL);
    strftime(t, sizeof t, "%H:%M:%S ", localtime(&now));
  }
  bool result = line[0] == ' ';
  bool bad = strstr(line, "failed") || strstr(line, "ERROR");
  lcd.setTextColor(bad ? TFT_RED : result ? TFT_GREEN : TFT_LIGHTGREY);
  lcd.print(t);
  lcd.println(line);
  lcd.setTextColor(TFT_WHITE);
}

/* ------------------------------------------------------------ main */

void setup()
{
  Wire1.setSDA(SB_SDA);
  Wire1.setSCL(SB_SCL);
  Wire1.begin();
  Wire1.setClock(10000);
  while (esc_pressed()) ;                   /* drain keys left from the menu */

  lcd.init();
  lcd.setRotation(4);
  lcd.fillScreen(TFT_BLACK);
  lcd.setTextScroll(true);
  lcd.setScrollRect(0, 0, lcd.width(), lcd.height());
  lcd.setTextColor(TFT_GREEN);
  lcd.println("PicoCalc FTP server   (Esc: menu)");
  lcd.setTextColor(TFT_WHITE);

  if (!sd.begin(SS_PIN, SD_SCK_MHZ(20)))
    wait_or_menu("SD card error.");

  load_settings();
  if (!ssid[0])
    wait_or_menu("No WiFi settings: put ssid= and password=\n"
                 "in /config_ftp.conf (or settime.cfg).");
  lcd.print("Settings: ");
  lcd.println(cfg_used);

  lcd.print("WiFi: joining ");
  lcd.println(ssid);
  WiFi.begin(ssid, password);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED)
  {
    for (int i = 0; i < 10; i++) { if (esc_pressed()) return_to_menu(); delay(50); }
    lcd.print(".");
    if (++tries % 20 == 0)
    {
      lcd.println(" retrying");
      WiFi.begin(ssid, password);
    }
  }
  lcd.println();
  WiFi.noLowPowerMode();                    /* power saving delays packets enough to time out transfers */

  set_clock();

  ftpSrv = FtpServer(21, 21000);
  ftpSrv.setLogCallback(ftp_log);
  ftpSrv.begin(ftp_user, ftp_password);

  lcd.println();
  lcd.setTextColor(TFT_YELLOW);
  lcd.print("ftp://");
  lcd.println(WiFi.localIP());
  lcd.setTextColor(TFT_WHITE);
  lcd.print("User: ");
  lcd.print(ftp_user);
  lcd.print("   Password: ");
  lcd.println(ftp_password);
  lcd.println("One connection at a time (set your");
  lcd.println("client's connection limit to 1).");
  lcd.println();
  lcd.println("Esc: back to the menu (wait for");
  lcd.println("transfers to finish first).");
  lcd.println();
  lcd.println("Commands from your client appear below.");
}

void loop()
{
  ftpSrv.handleFTP();
  static uint32_t next;
  if ((int32_t)(millis() - next) >= 0)
  {
    next = millis() + 50;
    if (esc_pressed()) return_to_menu();
  }
}
