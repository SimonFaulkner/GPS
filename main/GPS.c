// GPS module (tracker and/or display module)
// Copyright (c) 2019 Adrian Kennard, Andrews & Arnold Limited, see LICENSE file (GPL)
static const char TAG[] = "GPS";

// TODO
// Settings via UDP?
// Better roaming operator selection? Maybe signal strength, or retries of each?
// AT command wait for OK/ERROR

#include "revk.h"
#include <esp32/aes.h>
#include <driver/i2c.h>
#include <driver/uart.h>
#include <math.h>
#include "oled.h"
#include "esp_sntp.h"
#include "revkgps.h"
#include "owb.h"
#include "owb_rmt.h"
#include "ds18b20.h"
extern void hmac_sha256 (const uint8_t * key, size_t key_len, const uint8_t * data, size_t data_len, uint8_t * mac);

#define settings	\
	s8(oledsda,-1)	\
	s8(oledscl,-1)	\
	s8(oledaddress,0x3D)	\
	u8(oledcontrast,127)	\
	b(oledflip,N)	\
	bl(gpsdebug,N)	\
	s8(gpspps,-1)	\
	s8(gpsuart,1)	\
	s8(gpsrx,-1)	\
	s8(gpstx,-1)	\
	s8(gpsfix,-1)	\
	s8(gpsen,-1)	\
	s8(ds18b20,-1)	\
	u16(mtu,1488)	\
      	bl(atdebug,N)    \
        s8(atuart,2)	\
        u32(atbaud,115200)	\
        u32(gpsbaud,115200)	\
        s8(attx,-1)	\
        s8(atrx,-1)	\
        s8(atkey,-1)	\
        s8(atrst,-1)	\
        s8(atpwr,-1)	\
	u32(fixms,1000) \
	bl(fixdump,N)	\
	bl(fixdebug,N)	\
	b(battery,Y)	\
	u32(interval,600)\
	u32(keepalive,0)\
	u32(secondcm,10)\
        u32(altscale,10)\
	s(apn,"mobiledata")\
	s(operator,"O2")\
	s(loghost,"mqtt.revk.uk")\
	u32(logport,6666)\
	h(auth)		\
	u8(sun,0)	\
	u32(logslow,0)	\
	u32(logfast,0)	\
	b(navstar,Y)	\
	b(glonass,Y)	\
	b(galileo,Y)	\
	b(waas,Y)	\
	b(sbas,Y)	\
	b(qzss,N)	\
	b(aic,Y)	\
	b(easy,Y)	\
	b(mph, Y)	\
	u8(datafix,0x07)\
	b(datamargin,Y) \
	b(datatemp,Y)	\
	b(walking,N)	\
	b(flight,N)	\
	b(balloon,N)	\
	u8(refkmh,5)	\
	u8(lightmin,30)	\
	s8(light,-1)	\

#define u32(n,d)	uint32_t n;
#define u16(n,d)	uint16_t n;
#define s8(n,d)	int8_t n;
#define u8(n,d)	uint8_t n;
#define b(n,d) uint8_t n;
#define bl(n,d) uint8_t n;
#define h(n) uint8_t *n;
#define s(n,d) char * n;
settings
#undef u16
#undef u32
#undef s8
#undef u8
#undef bl
#undef b
#undef h
#undef s
float speed = 0;
float bearing = 0;
float lat = 0;
float lon = 0;
float alt = 0;
float gsep = 0;
float pdop = 0;
float hdop = 0;
float hepe = 0;
float vepe = 0;
float vdop = 0;
float course = 0;
uint8_t sats = 0;
uint8_t satsp = 0;
uint8_t satsl = 0;
uint8_t satsa = 0;
uint8_t fixtype = 0;
uint8_t fixmode = 0;
int8_t moving = 0;              // We are moving
int8_t mobile = 0;              // Mobile data on line
int8_t gotfix = 0;
int8_t lonforce = 0;
int8_t latforce = 0;
int8_t altforce = 0;
int8_t timeforce = 0;
int8_t speedforce = 0;
int8_t courseforce = 0;
int8_t hepeforce = 0;
int8_t vepeforce = 0;
int8_t hdopforce = 0;
int8_t pdopforce = 0;
int8_t vdopforce = 0;
volatile int8_t gpsstarted = 0;
char iccid[22] = { };
char imei[22] = { };

#define MAX_OWB 8
#define DS18B20_RESOLUTION   (DS18B20_RESOLUTION_12_BIT)
int8_t num_owb = 0;
OneWireBus *owb = NULL;
owb_rmt_driver_info rmt_driver_info;
DS18B20_Info *ds18b20s[MAX_OWB] = { 0 };

float tempc = -999;

#define MINL	0.1
time_t gpszda = 0;              // Last ZDA

unsigned int MAXFIX = 10000;    // Large memory max fix
#define MAXFIXLOW	1000    // Small memory max fix
#define	FIXALLOW	 100    // Allow time to process fixes
#define MAXDATA (mtu-28)        // SIM800 says 1472 allowed but only 1460 works FFS
unsigned int MAXTRACK = 1024;   // Large memory history
#define	MAXTRACKLOW	50      // Small memory history

float ascale = 1.0 / ASCALE;    // Alt scale default
volatile time_t fixbase = 0;    // Base time for fixtime
volatile time_t fixend = 0;     // End time for period covered (set on fixsend set, fixbase set to this on fixdelete done)
typedef struct fix_s fix_t;
#define ALTBASE	400             // Making alt unsigned as stored by allowing for -400m
struct fix_s
{                               // 12 byte fix data
   int lat;                     // min*DSCALE
   int lon;                     // min*DSCALE
   uint16_t tim;                // Time (TSCALE)
   uint16_t dist;               // RDP distance (MSCALE)
   uint16_t alt;                // Alt (ascale and offset, hence unsigned)
   uint8_t sats:6;              // Number of sats
   uint8_t dgps:1;              // DGPS
   uint8_t keep:1;              // Keep (RDP algorithm)
   uint8_t hepe;                // ESCALE
};
fix_t *fix = NULL;
unsigned int fixnext = 0;       // Next fix to store
volatile int fixsave = -1;      // Time to save fixes (-1 means not, so we do a zero fix at start)
volatile int fixdelete = -1;    // Delete this many fixes from start on next fix (-1 means not delete), and update trackbase
volatile char fixnow = 0;       // Force fix

uint8_t **track = NULL;
int *tracklen = NULL;

volatile unsigned int tracki = 0,
   tracko = 0;
volatile char trackmqtt = 0;
volatile uint32_t trackbase = 0;        // Send tracking for records after this time
volatile char trackfirst = 0;
SemaphoreHandle_t track_mutex = NULL;
void trackreset (time_t reference);

SemaphoreHandle_t cmd_mutex = NULL;
SemaphoreHandle_t ack_semaphore = NULL;
int pmtk = 0;                   // Waiting ack
SemaphoreHandle_t at_mutex = NULL;

#include "day.h"
#include "night.h"

time_t sun_find_crossing (time_t start_time, double latitude, double longitude, double wanted_altitude);
void sun_position (double t, double latitude, double longitude, double *altitudep, double *azimuthp);

#define DEGS_PER_RAD             (180 / M_PI)
#define SECS_PER_DAY             86400

#define MEAN_ANOMOLY_C           6.24005822136
#define MEAN_ANOMOLY_K           0.01720196999
#define MEAN_LONGITUDE_C         4.89493296685
#define MEAN_LONGITUDE_K         0.01720279169
#define LAMBDA_K1                0.03342305517
#define LAMBDA_K2                0.00034906585
#define MEAN_OBLIQUITY_C         0.40908772337
#define MEAN_OBLIQUITY_K         -6.28318530718e-09

#define MAX_ERROR                0.0000001

#define SUN_SET                         (-50.0/60.0)
#define SUN_CIVIL_TWILIGHT              (-6.0)
#define SUN_NAUTICAL_TWILIGHT           (-12.0)
#define SUN_ASTRONOMICAL_TWILIGHT       (-18.0)

time_t
sun_rise (int y, int m, int d, double latitude, double longitude, double sun_altitude)
{
 struct tm tm = { tm_mon: m - 1, tm_year: y - 1900, tm_mday: d, tm_hour:6 - longitude / 15 };
   return sun_find_crossing (mktime (&tm), latitude, longitude, sun_altitude);
}

time_t
sun_set (int y, int m, int d, double latitude, double longitude, double sun_altitude)
{
 struct tm tm = { tm_mon: m - 1, tm_year: y - 1900, tm_mday: d, tm_hour:18 - longitude / 15 };
   return sun_find_crossing (mktime (&tm), latitude, longitude, sun_altitude);
}

time_t
sun_find_crossing (time_t start_time, double latitude, double longitude, double wanted_altitude)
{
   double t,
     last_t,
     new_t,
     altitude,
     error,
     last_altitude;
   time_t result;
   int try = 10;

   last_t = (double) start_time;
   sun_position (last_t, latitude, longitude, &last_altitude, NULL);
   t = last_t + 1;
   do
   {
      if (!try--)
         return 0;              // Not found
      sun_position (t, latitude, longitude, &altitude, NULL);
      error = altitude - wanted_altitude;
      result = (time_t) (0.5 + t);

      new_t = t - error * (t - last_t) / (altitude - last_altitude);
      last_t = t;
      last_altitude = altitude;
      t = new_t;
   } while (fabs (error) > MAX_ERROR);
   return result;
}

void
sun_position (double t, double latitude, double longitude, double *altitudep, double *azimuthp)
{
   struct tm tm;
   time_t j2000_epoch;

   double latitude_offset;      // Site latitude offset angle (NORTH = +ve)
   double longitude_offset;     // Site longitude offset angle (EAST = +ve)
   double j2000_days;           // Time/date from J2000.0 epoch (Noon on 1/1/2000)
   double clock_angle;          // Clock time as an angle
   double mean_anomoly;         // Mean anomoly angle
   double mean_longitude;       // Mean longitude angle
   double lambda;               // Apparent longitude angle (lambda)
   double mean_obliquity;       // Mean obliquity angle
   double right_ascension;      // Right ascension angle
   double declination;          // Declination angle
   double eqt;                  // Equation of time angle
   double hour_angle;           // Hour angle (noon = 0, +ve = afternoon)
   double altitude;
   double azimuth;

   latitude_offset = latitude / DEGS_PER_RAD;
   longitude_offset = longitude / DEGS_PER_RAD;

//   printf("lat %f, long %f\n", latitude_offset * DEGS_PER_RAD, longitude_offset * DEGS_PER_RAD);

   // Calculate clock angle based on UTC unixtime of user supplied time
   clock_angle = 2 * M_PI * fmod (t, SECS_PER_DAY) / SECS_PER_DAY;

   // Convert localtime 'J2000.0 epoch' (noon on 1/1/2000) to unixtime
   tm.tm_sec = 0;
   tm.tm_min = 0;
   tm.tm_hour = 12;
   tm.tm_mday = 1;
   tm.tm_mon = 0;
   tm.tm_year = 100;
   tm.tm_wday = tm.tm_yday = tm.tm_isdst = 0;
   j2000_epoch = mktime (&tm);

   j2000_days = (double) (t - j2000_epoch) / SECS_PER_DAY;

   // Calculate mean anomoly angle (g)
   // [1] g = g_c + g_k * j2000_days
   mean_anomoly = MEAN_ANOMOLY_C + MEAN_ANOMOLY_K * j2000_days;

   // Calculate mean longitude angle (q)
   // [1] q = q_c + q_k * j2000_days
   mean_longitude = MEAN_LONGITUDE_C + MEAN_LONGITUDE_K * j2000_days;

   // Calculate apparent longitude angle (lambda)
   // [1] lambda = q + l_k1 * sin(g) + l_k2 * sin(2 * g)
   lambda = mean_longitude + LAMBDA_K1 * sin (mean_anomoly) + LAMBDA_K2 * sin (2 * mean_anomoly);

   // Calculate mean obliquity angle (e)     No trim - always ~23.5deg
   // [1] e = e_c + e_k * j2000_days
   mean_obliquity = MEAN_OBLIQUITY_C + MEAN_OBLIQUITY_K * j2000_days;

   // Calculate right ascension angle (RA)   No trim - atan2 does trimming
   // [1] RA = atan2(cos(e) * sin(lambda), cos(lambda))
   right_ascension = atan2 (cos (mean_obliquity) * sin (lambda), cos (lambda));

   // Calculate declination angle (d)        No trim - asin does trimming
   // [1] d = asin(sin(e) * sin(lambda))
   declination = asin (sin (mean_obliquity) * sin (lambda));

   // Calculate equation of time angle (eqt)
   // [1] eqt = q - RA
   eqt = mean_longitude - right_ascension;

   // Calculate sun hour angle (h)
   // h = clock_angle + long_o + eqt - PI
   hour_angle = clock_angle + longitude_offset + eqt - M_PI;

   // Calculate sun altitude angle
   // [2] alt = asin(cos(lat_o) * cos(d) * cos(h) + sin(lat_o) * sin(d))
   altitude =
      DEGS_PER_RAD * asin (cos (latitude_offset) * cos (declination) * cos (hour_angle) +
                           sin (latitude_offset) * sin (declination));

   // Calculate sun azimuth angle
   // [2] az = atan2(sin(h), cos(h) * sin(lat_o) - tan(d) * cos(lat_o))
   azimuth =
      DEGS_PER_RAD * atan2 (sin (hour_angle), cos (hour_angle) * sin (latitude_offset) - tan (declination) * cos (latitude_offset));

   if (altitudep)
      *altitudep = altitude;
   if (azimuthp)
      *azimuthp = azimuth;
}

/***************************************************************************/
/* References for equations:-

[1] U.S. Naval Observatory Astronomical Applications Department
    http://aa.usno.navy.mil/AA/faq/docs/SunApprox.html

[2] Astronomical Formulae for Calculators, Jean Meeus, Page 44

[3] Ben Mack, 'Tate - louvre angle calcs take 3', 10/12/1999

*/
/***************************************************************************/

void
gpscmd (const char *fmt, ...)
{                               // Send command to UART
   if (pmtk)
      xSemaphoreTake (ack_semaphore, 1000 * portTICK_PERIOD_MS);        // Wait for ACK from last command
   char s[100];
   va_list ap;
   va_start (ap, fmt);
   vsnprintf (s, sizeof (s) - 5, fmt, ap);
   va_end (ap);
   uint8_t c = 0;
   char *p;
   for (p = s + 1; *p; p++)
      c ^= *p;
   if (gpsdebug)
      revk_info ("gpstx", "%s", s);
   if (*s == '$')
      p += sprintf (p, "*%02X\r\n", c); // We allowed space
   xSemaphoreTake (cmd_mutex, portMAX_DELAY);
   uart_write_bytes (gpsuart, s, p - s);
   xSemaphoreGive (cmd_mutex);
   if (!strncmp (s, "$PMTK", 5))
   {
      xSemaphoreTake (ack_semaphore, 0);
      pmtk = atoi (s + 5);
   } else
      pmtk = 0;
}

#define ATBUFSIZE 2000
char *atbuf = NULL;
int
atcmd (const void *cmd, int t1, int t2)
{
   if (attx < 0 || atrx < 0)
      return 0;
   xSemaphoreTake (at_mutex, portMAX_DELAY);
   if (cmd)
   {
      uart_write_bytes (atuart, cmd, strlen ((char *) cmd));
      uart_write_bytes (atuart, "\r", 1);
      if (atdebug)
         revk_info ("attx", "%s", cmd);
   }
   int l = 0;
   while (1)
   {
      l = uart_read_bytes (atuart, (void *) atbuf, 1, (t1 ? : 100) / portTICK_PERIOD_MS);
      if (l > 0)
      {                         // initial response started
         int l2 = uart_read_bytes (atuart, (void *) atbuf + l, ATBUFSIZE - l - 1, 10 / portTICK_PERIOD_MS);
         if (l2 > 0 && t2)
         {                      // End of initial response
            l += l2;
            l2 = uart_read_bytes (atuart, (void *) atbuf + l, 1, t2 / portTICK_PERIOD_MS);
            if (l2 > 0)
            {                   // Secondary response started
               l2 = uart_read_bytes (atuart, (void *) atbuf + l, ATBUFSIZE - l - 1, 10 / portTICK_PERIOD_MS);
               if (l2 > 0)
                  l += l2;      // End of secondary response
               else
                  l = l2;
            }
         } else
            l = l2;
      }
      if (l >= 0)
      {
         atbuf[l] = 0;
      } else
         atbuf[0] = 0;
      char *p = atbuf;
      while (*p && *p < ' ')
         p++;
      char *e = p;
      while (*e && *e >= ' ')
         e++;
      while (*e && *e < ' ')
         e++;
      if (!*e && (!strncmp (p, "Call Ready", 10) || !strncmp (p, "SMS Ready", 9) || !strncmp (p, "+CFUN:", 6)))
      {
         if (atdebug)
            revk_info ("ignore", "%s", atbuf);
         continue;              // Skip known single line async messages we can ignore
      }
      if (l && atdebug)
         revk_info ("atrx", "%s", atbuf);
      break;
   }
   xSemaphoreGive (at_mutex);
   return l;
}

void
lograte (int rate)
{
   static int lastrate = -1;
   if (lastrate == rate)
      return;
   if (!rate)
      gpscmd ("$PMTK185,1");    // Stop log
   else
   {
      if (lastrate <= 0)
         gpscmd ("$PMTK185,0"); // Start log
      gpscmd ("$PMTK187,1,%d", rate);
   }
   lastrate = rate;
}

const char *
app_command (const char *tag, unsigned int len, const unsigned char *value)
{
   if (!strcmp (tag, "test"))
   {
      trackmqtt = 1 - trackmqtt;        // Switch for testing
      return "";
   }
   if (!strcmp (tag, "contrast"))
   {
      oled_set_contrast (atoi ((char *) value));
      return "";                // OK
   }
   if (!strcmp (tag, "wifi"))
   {                            // WiFi connected, but not need for SNTP as we have GPS
      if (gpszda)
         sntp_stop ();
      return "";
   }
   if (!strcmp (tag, "disconnect"))
   {
      xSemaphoreTake (track_mutex, portMAX_DELAY);
      trackmqtt = 0;
      xSemaphoreGive (track_mutex);
      return "";
   }
   if (!strcmp (tag, "connect"))
   {
      xSemaphoreTake (track_mutex, portMAX_DELAY);
      trackmqtt = 1;
      xSemaphoreGive (track_mutex);
      if (logslow || logfast)
         gpscmd ("$PMTK183");   // Log status
      if (*iccid)
         revk_info ("iccid", "%s", iccid);
      if (*imei)
         revk_info ("imei", "%s", imei);
      if (tracki && (attx < 0 || atrx < 0))
         fixnow = 1;
      if (!auth || *auth <= 3 + 16)
         revk_error ("auth", "Authentication not set");
      return "";
   }
   if (!strcmp (tag, "status"))
   {
      gpscmd ("$PMTK183");      // Log status
      return "";
   }
   if (!strcmp (tag, "resend"))
   {                            // Resend log data
      if (len)
      {
         struct tm t = { };
         strptime ((char *) value, "%Y-%m-%d %H:%M:%S", &t);
         trackreset (mktime (&t));
      } else
         trackreset (0);
      return "";
   }
   if (!strcmp (tag, "fix") || !strcmp (tag, "upgrade") || !strcmp (tag, "restart"))
   {                            // Force fix dump now
      fixnow = 1;
      return "";
   }
   if (!strcmp (tag, "time"))
   {
      if (!len)
         timeforce = 0;
      else
      {
         timeforce = 1;
         struct tm t = { };
         char *z = strptime ((char *) value, "%Y-%m-%d %H:%M:%S", &t);
         if (!z || !*z)
            t.tm_isdst = -1;
         struct timeval v = { };
         v.tv_sec = mktime (&t);
         settimeofday (&v, NULL);
      }
      return "";
   }
#define force(x) if (!strcmp (tag, #x)) { if (!len) x##force = 0; else { x##force = 1; x = strtof ((char *) value, NULL); } return ""; }
   force (lat);
   force (lon);
   force (alt);
   force (course);
   force (speed);
   force (hdop);
   force (pdop);
   force (vdop);
   force (hepe);
   force (vepe);
#undef force
   if (!strcmp (tag, "gpstx") && len)
   {                            // Send arbitrary GPS command (do not include *XX or CR/LF)
      gpscmd ("%s", value);
      return "";
   }
   if (!strcmp (tag, "attx") && len)
   {                            // Send arbitrary AT command (do not include *XX or CR/LF)
      atcmd (value, 0, 0);
      return "";
   }
   if (!strcmp (tag, "status"))
   {
      gpscmd ("$PMTK18,1");     // Status log
      return "";
   }
   if (!strcmp (tag, "dump"))
   {
      gpscmd ("$PMTK622,1");    // Dump log
      return "";
   }
   if (!strcmp (tag, "erase"))
   {
      gpscmd ("$PMTK184,1");    // Erase
      return "";
   }
   if (!strcmp (tag, "hot"))
   {
      gpscmd ("$PMTK101");      // Hot start
      gpsstarted = 0;
      return "";
   }
   if (!strcmp (tag, "warm"))
   {
      gpscmd ("$PMTK102");      // Warm start
      gpsstarted = 0;
      return "";
   }
   if (!strcmp (tag, "cold"))
   {
      gpscmd ("$PMTK103");      // Cold start
      gpsstarted = 0;
      return "";
   }
   if (!strcmp (tag, "reset"))
   {
      gpscmd ("$PMTK104");      // Full cold start (resets to default settings including Baud rate)
      revk_restart ("GPS has been reset", 1);
      return "";
   }
   if (!strcmp (tag, "sleep"))
   {
      gpscmd ("$PMTK291,7,0,10000,1");  // Low power (maybe we need to drive EN pin?)
      return "";
   }
   if (!strcmp (tag, "version"))
   {
      gpscmd ("$PMTK605");      // Version
      return "";
   }
   return NULL;
}

static void
fixcheck (unsigned int fixtim)
{
   time_t now = time (0);
   if (!timeforce && gpszda && fixsave < 0 && fixdelete < 0
       && (fixnow || fixnext > MAXFIX - FIXALLOW || (now - fixbase >= interval) || fixtim >= 65000))
   {
      if (fixdebug)
      {
         if (fixnow)
            revk_info (TAG, "Fix forced (%u)", fixnext);
         else if (fixnext > MAXFIX - FIXALLOW)
            revk_info (TAG, "Fix space full (%u)", fixnext);
         else if (now - fixbase >= interval)
            revk_info (TAG, "Fix time expired %u (%u)", (unsigned int) (now - fixbase), fixnext);
         else if (fixtim >= 65000)
            revk_info (TAG, "Fix tim too high %u (%u)", fixtim, fixnext);
      }
      fixend = time (0);
      fixsave = fixnext;        // Save the fixes we have so far (more may accumulate whilst saving)
   }
}

static void
gps_init (void)
{                               // Set up GPS
   gpscmd ("$PMTK286,%d", aic ? 1 : 0); // AIC
   gpscmd ("$PMTK353,%d,%d,%d,0,0", navstar, glonass, galileo);
   gpscmd ("$PMTK352,%d", qzss ? 0 : 1);        // QZSS (yes, 1 is disable)
   gpscmd ("$PQTXT,W,0,1");     // Disable TXT
   gpscmd ("$PQEPE,W,1,1");     // Enable EPE
   gpscmd ("$PMTK886,%d", balloon ? 3 : flight ? 2 : walking ? 1 : 0);  // FR mode
   // Queries - responses prompt settings changes if needed
   gpscmd ("$PMTK414");         // Q_NMEA_OUTPUT
   gpscmd ("$PMTK400");         // Q_FIX
   gpscmd ("$PMTK401");         // Q_DGPS
   gpscmd ("$PMTK413");         // Q_SBAS
   gpscmd ("$PMTK869,0");       // Query EASY
   gpscmd ("$PMTK605");         // Q_RELEASE
   gpsstarted = 1;
}

static void
nmea (char *s)
{
   if (gpsdebug)
      revk_info ("gpsrx", "%s", s);
   if (!s || *s != '$' || !s[1] || !s[2] || !s[3])
      return;
   char *f[50];
   int n = 0;
   s++;
   while (n < sizeof (f) / sizeof (*f))
   {
      f[n++] = s;
      while (*s && *s != ',')
         s++;
      if (!*s || *s != ',')
         break;
      *s++ = 0;
   }
   if (!n)
      return;
   if (!gpsstarted && *f[0] == 'G' && !strcmp (f[0] + 2, "GGA") && (esp_timer_get_time () > 10000000 || !revk_offline ()))
      gpsstarted = -1;          // Time to send init
   if (!strcmp (f[0], "PMTK001") && n >= 3)
   {                            // ACK
      int tag = atoi (f[1]);
      if (pmtk && pmtk == tag)
      {                         // ACK received
         xSemaphoreGive (ack_semaphore);
         pmtk = 0;
      }
      int ok = atoi (f[2]);
      if (ok == 1)
         revk_error (TAG, "PMTK%d unsupported", tag);
      else if (ok == 2)
         revk_error (TAG, "PMTK%d failed", tag);
      return;
   }
   if (!strcmp (f[0], "PQTXT"))
      return;                   // ignore
   if (*f[0] == 'G' && !strcmp (f[0] + 2, "GLL"))
      return;                   // ignore
   if (*f[0] == 'G' && !strcmp (f[0] + 2, "RMC"))
      return;                   // ignore
   if (!strcmp (f[0], "PMTK010"))
      return;                   // Started, happens at end of init anyway
   if (!strcmp (f[0], "PMTK011"))
      return;                   // Message! Ignore
   if (!strcmp (f[0], "PQEPE") && n >= 3)
   {                            // Estimated position error
      if (!hepeforce)
         hepe = strtof (f[1], NULL);
      if (!vepeforce)
         vepe = strtof (f[2], NULL);
      return;
   }
   if (!strcmp (f[0], "PMTK869") && n >= 4)
   {                            // Set EASY
      if (atoi (f[1]) == 2 && atoi (f[2]) != easy)
      {
         if (fixdebug)
            revk_info (TAG, "Setting EASY %s  (%s days)", easy ? "on" : "off", f[3]);
         gpscmd ("$PMTK869,1,%d", easy ? 1 : 0);
      }
      return;
   }
   if (!strcmp (f[0], "PMTK513") && n >= 2)
   {                            // Set SBAS
      if (atoi (f[1]) != sbas)
      {
         if (fixdebug)
            revk_info (TAG, "Setting SBAS %s", sbas ? "on" : "off");
         gpscmd ("$PMTK313,%d", sbas ? 1 : 0);
      }
      return;
   }
   if (!strcmp (f[0], "PMTK501") && n >= 2)
   {                            // Set DGPS
      if (atoi (f[1]) != ((sbas || waas) ? 2 : 0))
      {
         if (fixdebug)
            revk_info (TAG, "Setting DGPS %s", (sbas || waas) ? "on" : "off");
         gpscmd ("$PMTK301,%d", (sbas || waas) ? 2 : 0);
      }
      return;
   }
   if (!strcmp (f[0], "PMTK500") && n >= 2)
   {                            // Fix rate
      if (atoi (f[1]) != fixms)
      {
         if (fixdebug)
            revk_info (TAG, "Setting fix rate %dms", fixms);
         gpscmd ("$PMTK220,%d", fixms);
      }
      return;
   }
   if (!strcmp (f[0], "PMTK705") && n >= 2)
   {
      revk_info (TAG, "GPS version %s", f[1]);
      return;
   }
   if (!strcmp (f[0], "PMTK514") && n >= 2)
   {
      unsigned int rates[19] = { };
      rates[2] = (1000 / fixms ? : 1);  // VTG
      rates[3] = 1;             // GGA
      rates[4] = (10000 / fixms ? : 1); // GSA
      rates[5] = (10000 / fixms ? : 1); // GSV
      rates[17] = (10000 / fixms ? : 1);        // ZDA
      int q;
      for (q = 0; q < sizeof (rates) / sizeof (rates) && rates[q] == (1 + q < n ? atoi (f[1 + q]) : 0); q++);
      if (q < sizeof (rates) / sizeof (rates))
      {                         // Set message rates
         if (fixdebug)
            revk_info (TAG, "Setting message rates");
         gpscmd ("$PMTK314,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
                 rates[0], rates[1], rates[2], rates[3], rates[4], rates[5], rates[6], rates[7], rates[8], rates[9], rates[10],
                 rates[11], rates[12], rates[13], rates[14], rates[15], rates[16], rates[17], rates[18], rates[19]);
      }
      return;
   }
   if (*f[0] == 'G' && !strcmp (f[0] + 2, "GGA") && n >= 14)
   {                            // Fix: $GPGGA,093644.000,5125.1569,N,00046.9708,W,1,09,1.06,100.3,M,47.2,M,,
      if (strlen (f[1]) >= 10 && strlen (f[2]) >= 9 && strlen (f[4]) >= 10)
      {
         fixtype = atoi (f[6]);
         int s = atoi (f[7]);
         if (s != sats)
         {
            sats = s;
            if (fixdebug)
               revk_info (TAG, "Sats %d (NAVSTAR %d, GLONASS %d, GALILEO %d) type=%d mode=%d hepe=%.1f vepe=%.1f", sats, satsp,
                          satsl, satsa, fixtype, fixmode, hepe, vepe);
         }
         if (!altforce)
            alt = strtof (f[9], NULL);
         gsep = strtof (f[10], NULL);
         if (!latforce)
            lat = ((f[2][0] - '0') * 10 + f[2][1] - '0' + strtof (f[2] + 2, NULL) / 60) * (f[3][0] == 'N' ? 1 : -1);
         if (!lonforce)
            lon =
               ((f[4][0] - '0') * 100 + (f[4][1] - '0') * 10 + f[4][2] - '0' + strtof (f[4] + 3, NULL) / 60) * (f[5][0] ==
                                                                                                                'E' ? 1 : -1);
         if (!hdopforce)
            hdop = strtof (f[8], NULL);
         gotfix = 1;
         if (gpszda && fixtype)
         {                      // Store fix data
            char *p = f[2];
            int s = DSCALE;
            int fixlat = (((p[0] - '0') * 10 + p[1] - '0') * 60 + (p[2] - '0') * 10 + p[3] - '0') * s;
            p += 5;
            while (s > 1 && isdigit ((int) *p))
            {
               s /= 10;
               fixlat += s * (*p++ - '0');
            }
            if (f[3][0] == 'S')
               fixlat = 0 - fixlat;
            s = DSCALE;
            p = f[4];
            int fixlon = (((p[0] - '0') * 10 + p[1] - '0') * 60 + (p[2] - '0') * 60 + (p[3] - '0') * 10 + p[4] - '0') * s;
            p += 6;
            while (s > 1 && isdigit ((int) *p))
            {
               s /= 10;
               fixlon += s * (*p++ - '0');
            }
            if (f[5][0] == 'W')
               fixlon = 0 - fixlon;
            s = TSCALE;
            p = f[1];
            unsigned int fixtim =
               ((((p[0] - '0') * 10 + p[1] - '0') * 60 + (p[2] - '0') * 10 + p[3] - '0') * 60 + (p[4] - '0') * 10 + p[5] - '0') * s;
            p += 7;
            while (s > 1 && isdigit ((int) *p))
            {
               s /= 10;
               fixtim += s * (*p++ - '0');
            }
            if (fixtim / TSCALE + 100 < (gpszda % 86400))
               fixtim += 86400 * TSCALE;        // Day wrap
            fixtim -= (fixbase - gpszda / 86400 * 86400) * TSCALE;
            int fixalt = round ((alt + ALTBASE) / ascale);      // Offset and scale to store in 16 bits
            if (fixalt < 0)
               fixalt = 0;      // Range to 16 bits
            else if (fixalt > 65535)
               fixalt = 65535;
            if (fixmode < 3)
               fixalt = 0;
            int fixhepe = round (hepe * ESCALE);
            if (fixhepe > 255)
               fixhepe = 255;   // Limit
            int fixdiff (fix_t * a, fix_t * b)
            {                   // Different (except for time)
               if (a->lat != b->lat)
                  return 1;
               if (a->lon != b->lon)
                  return 2;
               if (a->alt != b->alt)
                  return 3;
               if (a->sats != b->sats)
                  return 4;
               if (a->hepe != b->hepe)
                  return 5;
               if (a->dgps != b->dgps)
                  return 6;
               return 0;
            }
            if (fixnext < MAXFIX && (fixnext < 2 || moving))
            {
               fix[fixnext].keep = 0;
               fix[fixnext].dist = 0;
               fix[fixnext].tim = fixtim;
               fix[fixnext].alt = fixalt;
               fix[fixnext].lat = fixlat;
               fix[fixnext].lon = fixlon;
               fix[fixnext].sats = sats;
               fix[fixnext].dgps = (fixtype == 2 ? 1 : 0);
               fix[fixnext].hepe = fixhepe;
               if (fixnext > 1 && fixnext > fixsave + 1 && !fixdiff (&fix[fixnext - 1], &fix[fixnext])
                   && !fixdiff (&fix[fixnext - 2], &fix[fixnext - 1]))
                  fix[fixnext - 1].tim = fixtim;        // Skip intermediate identical fix
               else
               {                // Save
                  if (fixdump)
                     revk_info (TAG, "fix:%u tim=%u/%d lat=%d/60 lon=%d/60 alt=%d*%.1f", fixnext, fixtim, TSCALE, fixlat, fixlon,
                                fixalt, ascale);
                  fixnext++;
               }
               fixcheck (fixtim);
            }
         }
      }
      if (fixdelete >= 0)
      {                         // Move back fixes
         unsigned int n,
           p = 0;
         int diff = fixend - fixbase;
         //if (fixdebug) revk_info(TAG, "Fix deleted %d, adjust %d", fixdelete, diff);
         for (n = fixdelete; n < fixnext; n++)
         {
            fix[p] = fix[n];
            fix[p].tim -= diff * TSCALE;
            p++;
         }
         fixbase = fixend;      // New base
         fixnext = p;
         fixdelete = -1;
      }
      return;
   }
   if (*f[0] == 'G' && !strcmp (f[0] + 2, "ZDA") && n >= 5)
   {                            // Time: $GPZDA,093624.000,02,11,2019,,
      if (strlen (f[1]) == 10 && !timeforce && atoi (f[4]) > 2000)
      {
         struct tm t = { };
         struct timeval v = { };
         t.tm_year = atoi (f[4]) - 1900;
         t.tm_mon = atoi (f[3]) - 1;
         t.tm_mday = atoi (f[2]);
         t.tm_hour = (f[1][0] - '0') * 10 + f[1][1] - '0';
         t.tm_min = (f[1][2] - '0') * 10 + f[1][3] - '0';
         t.tm_sec = (f[1][4] - '0') * 10 + f[1][5] - '0';
         v.tv_usec = atoi (f[1] + 7) * 1000;
         v.tv_sec = mktime (&t);
         if (!gpszda)
         {
            sntp_stop ();
            if (fixdebug)
               revk_info (TAG, "Set clock %s-%s-%s %s", f[4], f[3], f[2], f[1]);
            fixbase = v.tv_sec;
            fixend = v.tv_sec;
            fixsave = 0;        // Send a null fix
         }
         gpszda = v.tv_sec;
         settimeofday (&v, NULL);
      }
      return;
   }
   if (*f[0] == 'G' && !strcmp (f[0] + 2, "VTG") && n >= 10)
   {
      if (!courseforce)
         course = strtof (f[1], NULL);
      if (!speedforce)
         speed = strtof (f[7], NULL);
      // Are we moving?
      if (speed < hdop * hdop)
      {
         if (moving)
         {
            if (fixdebug)
               revk_info (TAG, "Not moving %.1fkm/h %.2f HDOP", speed, hdop);
            moving = 0;         // Stopped moving
            lograte (logslow);
         }
      } else if (speed > 1 && fixmode > 1 && hdop && speed > (float) refkmh * hdop * hdop)
      {
         if (!moving)
         {
            if (fixdebug)
               revk_info (TAG, "Moving %.1fkm/h %.2f HDOP", speed, hdop);
            moving = 1;         // Started moving
            lograte (logfast);
         }
      }
      return;
   }
   if (*f[0] == 'G' && !strcmp (f[0] + 2, "GSA") && n >= 18)
   {
      fixmode = atoi (f[2]);
      if (!pdopforce)
         pdop = strtof (f[15], NULL);
      if (!vdopforce)
         vdop = strtof (f[17], NULL);
      return;
   }
   if (*f[0] == 'G' && !strcmp (f[0] + 2, "GSV") && n >= 4)
   {
      int n = atoi (f[3]);
      if (f[0][1] == 'P')
         satsp = n;
      else if (f[0][1] == 'L')
         satsl = n;
      else if (f[0][1] == 'A')
         satsa = n;
      return;
   }
   if (!gpsdebug)
   {                            // Report unknown
      for (int q = 1; q < n; q++)
         f[q][-1] = ',';
      revk_error ("gpsrx", "$%s", f[0]);
   }
}

static void
display_task (void *p)
{
   p = p;
   while (1)
   {
      if (gpspps >= 0)
      {
         int try = 20;
         if (gpio_get_level (gpspps))
            usleep (800000);    // In pulse
         while (!gpio_get_level (gpspps) && try-- > 0)
            usleep (10000);
      } else
         sleep (1);
      oled_lock ();
      int y = CONFIG_OLED_HEIGHT,
         x = 0;
      time_t now = time (0) + 1;
      struct tm t;
      localtime_r (&now, &t);
      if (t.tm_year > 100)
      {
         char temp[30];
         strftime (temp, sizeof (temp), "%F\004%T %Z", &t);
         oled_text (1, 0, 0, temp);
      }
      y -= 10;
      oled_text (1, 0, y, "Fix: %s %2d\002sat%s %s", revk_offline ()? " " : tracko == tracki ? "*" : "+", sats,
                 sats == 1 ? " " : "s", mobile ? tracko == tracki ? "*" : "+" : " ");
      oled_text (1, CONFIG_OLED_WIDTH - 6 * 4, y, "%c%c%c%c",   //
                 navstar ? satsp ? 'P' : '-' : ' ',     // G[P]S (NAVSTAR(
                 glonass ? satsl ? 'L' : '-' : ' ',     // G[L]ANOSS
                 galileo ? satsa ? 'A' : '-' : ' ',     // G[A]LILEO
                 fixtype == 2 ? 'D' : ((waas || sbas) && fixms >= 1000) ? '-' : ' ');   // DGPS
      y -= 3;                   // Line
      y -= 8;
      if (fixmode > 1)
         oled_text (1, 0, y, "Lat: %11.6f", lat);
      else
         oled_text (1, 0, y, "%16s", "");
      oled_text (1, CONFIG_OLED_WIDTH - 3 * 6, y, fixmode > 1 ? hepe ? "EPE" : "DOP" : "   ");
      y -= 8;
      if (fixmode > 1)
         oled_text (1, 0, y, "Lon: %11.6f", lon);
      else
         oled_text (1, 0, y, "%16s", "");
      if (fixmode > 1)
      {
         if (hepe > 999.9)
            oled_text (1, CONFIG_OLED_WIDTH - 5 * 6 - 2, y, "---.-m", hepe);
         else if (hepe)
            oled_text (1, CONFIG_OLED_WIDTH - 5 * 6 - 2, y, "%5.1fm", hepe);
         else
            oled_text (1, CONFIG_OLED_WIDTH - 5 * 6 - 2, y, "%6.2f", hdop);
      } else
         oled_text (1, CONFIG_OLED_WIDTH - 5 * 6 - 2, y, "     \002");
      y -= 8;
      if (fixmode >= 3)
         oled_text (1, 0, y, "Alt: %6.1fm", alt);
      else
         oled_text (1, 0, y, "%16s", "");
      if (fixmode >= 3)
      {
         if (vepe > 999.9)
            oled_text (1, CONFIG_OLED_WIDTH - 5 * 6 - 2, y, "---.-m", vepe);
         else if (vepe)
            oled_text (1, CONFIG_OLED_WIDTH - 5 * 6 - 2, y, "%5.1fm", vepe);
         else
            oled_text (1, CONFIG_OLED_WIDTH - 5 * 6 - 2, y, "%6.2f", vdop);
      } else
         oled_text (1, CONFIG_OLED_WIDTH - 5 * 6 - 2, y, "     \002");
      y -= 3;                   // Line
      if (gpszda && gotfix)
      {
         // Sun
         y -= 22;
         double sunalt,
           sunazi;
         sun_position (now, lat, lon, &sunalt, &sunazi);
         int o = 0;
         time_t rise,
           set;
         do
            rise = sun_rise (t.tm_year + 1900, t.tm_mon + 1, t.tm_mday + o++, lat, lon, sun ? : SUN_SET);
         while (rise <= now && o < 200);
         o = 0;
         do
            set = sun_set (t.tm_year + 1900, t.tm_mon + 1, t.tm_mday + o++, lat, lon, sun ? : SUN_SET);
         while (set + lightmin * 60 <= now && o < 200);
         if (rise > set)
            o = set - now;
         else
            o = rise - now;
         if (o >= 0 && rise > set)
         {
            oled_icon (0, y, day, 22, 21);
         } else
         {
            oled_icon (0, y, night, 22, 21);
            // Moon
#define LUNY 2551442.8768992    // Seconds per lunar cycle
            int s = now - 1571001050;   // Seconds since reference full moon
            int m = ((float) s / LUNY); // full moon count
            s -= (float) m *LUNY;       // seconds since full moon
            float phase = (float) s * M_PI * 2 / LUNY;
#define w (21.0/2.0)
            if (phase < M_PI)
            {                   // dim on right (northern hemisphere)
               float q = w * cos (phase);
               for (int Y = 0; Y < w * 2; Y++)
               {
                  float d = (float) Y + 0.5 - w;
                  float v = q * sqrt (1 - (float) d / w * (float) d / w) + w;
                  int l = ceil (v);
                  if (l)
                     oled_set (l - 1, y + Y, (1.0 - ((float) l - v)) * oled_get (l - 1, y + Y));
                  for (int X = l; X < CONFIG_OLED_WIDTH; X++)
                     oled_set (X, Y + y, (X + Y) & 1 ? 0 : oled_get (X, Y + y) >> 3);
               }
            } else
            {                   // dim on left (northern hemisphere)
               float q = -w * cos (phase);
               for (int Y = 0; Y < w * 2; Y++)
               {
                  float d = (float) Y + 0.5 - w;
                  float v = q * sqrt (1 - (float) d / w * (float) d / w) + w;
                  int r = floor (v);
                  if (r < w * 2)
                     oled_set (r, Y + y, (1.0 - (v - r)) * oled_get (r, Y + y));
                  for (int X = 0; X < r; X++)
                     oled_set (X, Y + y, (X + Y) & 1 ? 0 : oled_get (X, Y + y) >> 3);
               }
            }
#undef w
         }
         x = 22;
         {
            const char *s = "%3d";
            if (o < 0)
            {
               o = 0 - o;
               s = "%+3d";
            }
            if (light >= 0)
               gpio_set_level (light, rise > set || o < lightmin * 60 ? 1 : 0); // Light control
            if (o / 3600 < 1000)
            {                   // H:MM:SS
               x = oled_text (3, x, y, s, o / 3600);
               x = oled_text (2, x, y, ":%02d", o / 60 % 60);
               x = oled_text (1, x, y, ":%02d", o % 60);
            } else
            {                   // D HH:MM
               x = oled_text (3, x, y, s, o / 86400);
               x = oled_text (2, x, y, "\002%02d", o / 3600 % 24);
               x = oled_text (1, x, y, ":%02d", o / 60 % 60);
            }
         }
         // Sun angle
         x = CONFIG_OLED_WIDTH - 4 - 3 * 6;
         x = oled_text (1, x, y + 14, "%+3.0f", sunalt);
         x = oled_text (0, x, y + 17, "o");
         y -= 3;                // Line
      }
      // Speed
      y = 20;
      float s = speed;
      if (mph)
         s /= 1.609344;         // miles
      if (!moving)
         x = oled_text (5, 0, y, " -.-");
      else if (s >= 999)
         x = oled_text (5, 0, y, "\002---");
      else if (s >= 99.9)
         x = oled_text (5, 0, y, "\002%3.0f", s);
      else
         x = oled_text (5, 0, y, "%4.1f", s);
      oled_text (-1, CONFIG_OLED_WIDTH - 4 * 6, y + 2, "%4s", mph ? "mph" : "km/h");
      if (!moving)
         x = oled_text (-1, CONFIG_OLED_WIDTH - 3 * 6 - 4, y + 12, "---");
      else
         x = oled_text (-1, CONFIG_OLED_WIDTH - 3 * 6 - 4, y + 12, "%3.0f", course);
      x = oled_text (0, x, y + 12 + 3, "o");
      if (ds18b20 >= 0)
      {
         if (tempc < -9.9 || tempc > 99.9)
            x = oled_text (-2, CONFIG_OLED_WIDTH - 2 * 12, y + 24, "--");
         else
            x = oled_text (-2, CONFIG_OLED_WIDTH - 2 * 12, y + 24, "%2.0f", tempc);
      }
      oled_unlock ();
   }
}

void
trackreset (time_t reference)
{                               // Reset tracking
   xSemaphoreTake (track_mutex, portMAX_DELAY);
   if (reference > fixbase)
      reference = fixbase;      // safety net
   trackbase = reference;
   trackfirst = 1;
   if (tracki < MAXTRACK)
      tracko = 0;
   else
      tracko = tracki - MAXTRACK;
   xSemaphoreGive (track_mutex);
   if (fixdebug)
      revk_info (TAG, "Resend from %u", (unsigned int) reference);
}

unsigned int
encode (uint8_t * buf, unsigned int len, time_t ref)
{
   if (!auth || *auth <= 3 + 16)
      return 0;
   // Header
   buf[0] = VERSION;
   buf[1] = auth[1];
   buf[2] = auth[2];
   buf[3] = auth[3];
   buf[4] = ref >> 24;
   buf[5] = ref >> 16;
   buf[6] = ref >> 8;
   buf[7] = ref;
   while ((len & 0xF) != HEADLEN)
      buf[len++] = TAGF_PAD;
   {                            // Encrypt
      uint8_t iv[16] = { };
      memcpy (iv, buf, HEADLEN > sizeof (iv) ? sizeof (iv) : HEADLEN);
      esp_aes_context ctx;
      esp_aes_init (&ctx);
      esp_aes_setkey (&ctx, auth + 1 + 3, 128);
      esp_aes_crypt_cbc (&ctx, ESP_AES_ENCRYPT, len - HEADLEN, iv, buf + HEADLEN, buf + HEADLEN);
      esp_aes_free (&ctx);
   }
   {                            // HMAC
      uint8_t mac[32];
      hmac_sha256 (auth + 1 + 3 + 16, *auth - 3 - 16, buf, len, mac);
      memcpy (buf + len, mac, MACLEN);
      len += MACLEN;
   }
   return len;
}

int
tracknext (uint8_t * buf)
{                               // Check if tracking message to be sent, if so, pack and encrypt in to bug and return length, else 0. -1 if try again
   if (tracko >= tracki)
      return 0;                 // nothing to send
   xSemaphoreTake (track_mutex, portMAX_DELAY);
   int len = 0;
   if (tracko < tracki)
   {                            // Still something to send now we have semaphore
      if (tracko + MAXTRACK >= tracki)
      {                         // Not overrun (should not happen otherwise)
         len = tracklen[tracko % MAXTRACK];
         if (len)
         {                      // Has data (should not happen otherwise)
            uint8_t *src = track[tracko % MAXTRACK];
            uint32_t ts = (src[4] << 24) + (src[5] << 16) + (src[6] << 8) + src[7];
            if (ts >= trackbase)
            {                   // data to send
               if (trackfirst)
               {                // Send new trackbase
                  trackfirst = 0;
                  len = 8;
                  buf[len++] = 0;       // Period not applicable
                  buf[len++] = 0;
                  buf[len++] = TAGF_FIRST;
                  buf[len++] = ts >> 24;
                  buf[len++] = ts >> 16;
                  buf[len++] = ts >> 8;
                  buf[len++] = ts;
                  len = encode (buf, len, trackbase);
                  tracko--;     // Resend (as a period is covered by next packet)
               } else
                  memcpy (buf, src, len);       // Send message
            } else
               len = -1;        // Try again as we have not caught up
         }
      }
      tracko++;                 // Next
   }
   xSemaphoreGive (track_mutex);
   return len;
}

void
at_task (void *X)
{
   atbuf = malloc (ATBUFSIZE);
   while (1)
   {
      gpio_set_level (atpwr, 1);
      sleep (1);
      gpio_set_level (atkey, 0);
      sleep (1);
      gpio_set_level (atrst, 0);
      gpio_set_level (atkey, 1);
      sleep (1);
      gpio_set_level (atrst, 1);
      int try = 60;
      while ((atcmd ("AT", 0, 0) < 0 || (strncmp (atbuf, "AT", 2) && !strstr(atbuf, "OK"))) && --try > 0)
         sleep (1);
      if (try <= 0)
         continue;              // Cause power cycle and try again
      atcmd ("ATE0", 0, 0);
      atcmd (NULL, 0, 0);
      if (atcmd ("AT+GSN", 0, 0) > 0)
      {
         char *p = atbuf,
            *o = imei;
         while (*p && *p < ' ')
            p++;
         while (isdigit ((int) *p) && o < imei + sizeof (imei) - 1)
            *o++ = *p++;
         *o = 0;
         if (*imei)
            revk_info ("imei", "%s", imei);
      }
      if (atcmd ("AT+CCID", 0, 0) > 0)
      {
         char *p = atbuf,
            *o = iccid;
         while (*p && *p < ' ')
            p++;
         while (isdigit ((int) *p) && o < iccid + sizeof (iccid) - 1)
            *o++ = *p++;
         *o = 0;
         if (*iccid)
            revk_info ("iccid", "%s", iccid);
      }
      atcmd ("AT+CBC", 0, 0);
      time_t next = 0;
      try = 50;
      while (1)
      {
         mobile = 0;
         if (--try <= 0)
            break;              // Power cycle
         do
            atcmd (NULL, 1000, 0);
         while (time (0) < next);
         next = time (0) + 10;  // retry time
         if (atcmd ("AT+CIPSHUT", 2000, 0) < 0)
            continue;
         if (!strstr ((char *) atbuf, "SHUT OK"))
            continue;
         char roam = 0;
         while (--try > 0)
         {
            sleep (1);
            if (atcmd ("AT+CREG?", 1000, 1000) < 0)
               continue;
            if (!strstr ((char *) atbuf, "OK"))
               continue;
            if (strstr ((char *) atbuf, "+CREG: 0,5"))
            {
               roam = 1;
               break;
            }
            if (strstr ((char *) atbuf, "+CREG: 0,1"))
               break;
         }
         if (atcmd ("AT+COPS=?", 20000, 1000) < 0)      // Operator list
            continue;
         if (!strstr ((char *) atbuf, "OK"))
            continue;
         if (*operator&& strstr (atbuf, operator))
         {
            char temp[200];
            snprintf (temp, sizeof (temp), "AT+COPS=1,0,\"%s\"", operator);
            if (atcmd (temp, 10000, 1000) < 0)
               continue;
         }
         if (!strstr ((char *) atbuf, "OK"))
         {
            if (atcmd ("AT+COPS=0", 10000, 1000) < 0)   // Automatic selection
               continue;
            if (!strstr ((char *) atbuf, "OK"))
               continue;
         }
         if (atcmd ("AT+COPS?", 20000, 1000) < 0)       // Operator selected
            continue;
         if (!strstr ((char *) atbuf, "OK"))
            continue;
         {
            char temp[200];
            snprintf (temp, sizeof (temp), "AT+CSTT=\"%s\"", apn);
            if (atcmd (temp, 1000, 0) < 0)
               continue;
            if (!strstr ((char *) atbuf, "OK"))
               continue;
         }
         if (atcmd ("AT+CIICR", 60000, 0) < 0)
            continue;
         if (!strstr ((char *) atbuf, "OK"))
            continue;
         next = time (0) + 300; // Don't hammer mobile data connections
         if (atcmd ("AT+CIFSR", 20000, 0) < 0)
            continue;
         if (!strstr ((char *) atbuf, "."))
            continue;           // Yeh, not an OK after the IP!!! How fucking stupid
         {
            char temp[200];
            snprintf (temp, sizeof (temp), "AT+CIPSTART=\"UDP\",\"%s\",%d", loghost, logport);
            if (atcmd (temp, 1000, 10000) < 0)
               continue;
            if (!strstr ((char *) atbuf, "OK"))
               continue;
         }
         if (!strstr ((char *) atbuf, "CONNECT OK"))
            continue;
         mobile = 1;
         try = 50;
         revk_info (TAG, "Mobile connected%s", roam ? " (roaming)" : "");
         time_t ka = 0;
         while (1)
         {                      // Connected, send data as needed
            time_t now = time (0);
            int len = 0;
            uint8_t buf[MAXDATA];
            if (!trackmqtt && (len = tracknext (buf)) > 0)
            {
               char temp[30];
               snprintf (temp, sizeof (temp), "AT+CIPSEND=%d", len);
               if (atcmd (temp, 1000, 0) < 0 || !strstr (atbuf, ">"))
                  break;        // Failed
               uart_write_bytes (atuart, (void *) buf, len);
               if (atcmd (NULL, 10000, 0) < 0 || !strstr (atbuf, "SEND OK"))
                  break;        // Failed
               ka = now + keepalive;
            }
            if (len < 0)
               continue;        // try again
            if (!trackmqtt && keepalive && now > ka)
            {                   // Keep alives
               if (atcmd ("AT+CIPSEND=1", 1000, 0) < 0 || !strstr (atbuf, ">"))
                  break;        // Failed
               uint8_t v = VERSION;
               uart_write_bytes (atuart, (void *) &v, 1);
               if (atcmd (NULL, 10000, 0) < 0 || !strstr (atbuf, "SEND OK"))
                  break;        // Failed
               ka = now + keepalive;
            }
            len = atcmd (NULL, 1000, 0);        // Note, this causes 1 second delay between each message, which seems prudent
            if (len <= 0)
               continue;
            if (strstr (atbuf, "+PDP: DEACT"))
               break;
            if (!auth || *auth <= 3 + 16)
            {
               revk_error (TAG, "No auth to decode message");
               len = 0;         // No auth
            }
            // Note, it seems to truncate by a byte, FFS
            if (len >= HEADLEN + 16 + MACLEN && *atbuf == VERSION && atbuf[1] == auth[1] && atbuf[2] == auth[2]
                && atbuf[3] == auth[3])
            {                   // Rx?
               time_t ts = (atbuf[4] << 24) + (atbuf[5] << 16) + (atbuf[6] << 8) + atbuf[7];
               if (ts <= now + 5 && ts > now - 60)
               {
                  len = HEADLEN + MACLEN + (len - HEADLEN - MACLEN) / 16 * 16;  // AES block size
                  {             // HMAC
                     len -= MACLEN;
                     uint8_t mac[32];
                     hmac_sha256 (auth + 1 + 3 + 16, *auth - 3 - 16, (void *) atbuf, len, mac);
                     if (memcmp (mac, atbuf + len, MACLEN))
                     {
                        revk_error (TAG, "Bad HMAC (%u bytes)", len);
                        len = 0;        // bad HMAC
                     }
                  }
                  {             // Decrypt
                     uint8_t iv[16] = { };
                     memcpy (iv, atbuf, HEADLEN > sizeof (iv) ? sizeof (iv) : HEADLEN);
                     esp_aes_context ctx;
                     esp_aes_init (&ctx);
                     esp_aes_setkey (&ctx, auth + 1 + 3, 128);
                     esp_aes_crypt_cbc (&ctx, ESP_AES_DECRYPT, len - HEADLEN, iv, (void *) atbuf + HEADLEN,
                                        (void *) atbuf + HEADLEN);
                     esp_aes_free (&ctx);
                  }
                  {             // Process
                     uint8_t *p = (void *) atbuf + HEADLEN;
                     uint8_t *e = (void *) atbuf + len;
                     while (p < e)
                     {          // Process tags
                        unsigned int dlen = 0;
                        if (*p < 0x20)
                           dlen = 0;
                        else if (*p < 0x40)
                           dlen = 1;
                        else if (*p < 0x60)
                           dlen = 2;
                        else if (*p < 0x7F)
                           dlen = 4;
                        else
                           dlen = 3 + (p[2] << 8) + p[3];
                        if (*p == TAGT_FIX)
                           fixnow = 1;
                        else if (*p == TAGT_RESEND)
                           trackreset ((p[1] << 24) + (p[2] << 16) + (p[3] << 8) + p[4]);
                        p += 1 + dlen;
                     }
                  }
               }
            }
         }
         revk_info (TAG, "Mobile disconnected");
      }
   }
}

void
log_task (void *z)
{                               // Log via MQTT
   while (1)
   {
      int len = 0;
      uint8_t buf[MAXDATA];
      if (trackmqtt && (len = tracknext (buf)) > 0)
         revk_raw ("info", "udp", len, buf, 0);
      else if (len >= 0)
         sleep (1);             // Wait for next message to send
   }
}

void
nmea_task (void *z)
{
   uint8_t buf[1000],
    *p = buf;
   uint64_t timeout = esp_timer_get_time () + 10000000;
   while (1)
   {
      // Get line(s), the timeout should mean we see one or more whole lines typically
      int l = uart_read_bytes (gpsuart, p, buf + sizeof (buf) - p, 10 / portTICK_PERIOD_MS);
      if (l <= 0)
      {
         if (timeout && timeout < esp_timer_get_time ())
         {
            revk_restart ("GPS silent", 0);
            timeout = 0;
         }
         continue;
      }
      uint8_t *e = p + l;
      p = buf;
      while (p < e)
      {
         uint8_t *l = p;
         while (l < e && *l >= ' ')
            l++;
         if (l == e)
            break;
         if (*p == '$' && (l - p) >= 4 && l[-3] == '*' && isxdigit (l[-2]) && isxdigit (l[-1]))
         {
            // Checksum
            uint8_t c = 0,
               *x;
            for (x = p + 1; x < l - 3; x++)
               c ^= *x;
            if (((c >> 4) > 9 ? 7 : 0) + (c >> 4) + '0' != l[-2] || ((c & 0xF) > 9 ? 7 : 0) + (c & 0xF) + '0' != l[-1])
               revk_error (TAG, "[%.*s] (%02X)", l - p, p, c);
            else
            {                   // Process line
               timeout = esp_timer_get_time () + 10000000;
               l[-3] = 0;
               nmea ((char *) p);
            }
         } else if (l > p)
            revk_error (TAG, "[%.*s]", l - p, p);
         while (l < e && *l < ' ')
            l++;
         p = l;
      }
      if (p < e && (e - p) < sizeof (buf))
      {                         // Partial line
         memmove (buf, p, e - p);
         p = buf + (e - p);
         continue;
      }
      p = buf;                  // Start from scratch
   }
}

int
fixdistcmp (const void *a, const void *b)
{
   if (((fix_t *) a)->dist < ((fix_t *) b)->dist)
      return 1;
   if (((fix_t *) a)->dist > ((fix_t *) b)->dist)
      return -1;
   return 0;
}

int
fixtimcmp (const void *a, const void *b)
{
   if (((fix_t *) a)->tim < ((fix_t *) b)->tim)
      return -1;
   if (((fix_t *) a)->tim > ((fix_t *) b)->tim)
      return 1;
   return 0;
}

unsigned int
rdp (unsigned int H, unsigned int max, unsigned int *dlostp, unsigned int *dkeptp)
{                               // Reduce, non recursive
   unsigned int l = 0,
      h = H;                    // Progress
   fix[0].keep = 1;
   fix[H].keep = 1;
   uint64_t start = esp_timer_get_time ();
   while (l < H)
   {
      if (l == h)
         for (h++; h < H && !fix[h].keep; h++); // Find second half
      if (l + 1 == h)
      {                         // No points
         l++;
         continue;
      }
      fix_t *a = &fix[l];
      fix_t *b = &fix[h];
      // Centre - mainly to increase sig bits in floats by removing large fixed offset, but also for longitude metres base
      int clat = a->lat / 2 + b->lat / 2;
      int clon = a->lon / 2 + b->lon / 2;
      int calt = ((int) a->alt + (int) b->alt) / 2;
      int ctim = ((int) a->tim + (int) b->tim) / 2;
      float slon = 111111.0 * cos (M_PI * clat / 60.0 / 180.0 / DSCALE) / 60.0 / DSCALE;
      inline float x (fix_t * p)
      {
         return (float) (p->lon - clon) * slon;
      }
      inline float y (fix_t * p)
      {
         return (float) (p->lat - clat) * 111111.0 / 60.0 / DSCALE;
      }
      inline float z (fix_t * p)
      {
         if (!(datafix & TAGF_FIX_ALT) || !p->alt)
            return 0;           // Not considering alt
         return (float) (p->alt - calt) * ascale / (float) altscale; // altscale is adjust for point reduction
      }
      inline float t (fix_t * p)
      {
         return (float) ((int) p->tim - ctim) * secondcm / 100.0 / TSCALE;
      }
      inline float distsq (float dx, float dy, float dz, float dt)
      {                         // Distance in 4D space
         return dx * dx + dy * dy + dz * dz + dt * dt;
      }
      float DX = x (b) - x (a);
      float DY = y (b) - y (a);
      float DZ = z (b) - z (a);
      float DT = t (b) - t (a);
      float LSQ = distsq (DX, DY, DZ, DT);
      int bestn = -1;
      float best = 0;
      int n;
      for (n = l + 1; n < h; n++)
      {
         fix_t *p = &fix[n];
         if (p->tim)
         {
            float d = 0;
            if (!LSQ)
               d = distsq (x (p) - x (a), y (p) - y (a), z (p) - z (a), t (p) - t (a)); // Simple distance from point
            else
            {
               float T = ((x (p) - x (a)) * DX + (y (p) - y (a)) * DY + (z (p) - z (a)) * DZ + (t (p) - t (a)) * DT) / LSQ;
               d = distsq (x (a) + T * DX - x (p), y (a) + T * DY - y (p), z (a) + T * DZ - z (p), t (a) + T * DT - t (p));
            }
            if (bestn >= 0 && d <= best)
               continue;
            bestn = n;          // New furthest
            best = d;
         }
      }
      unsigned int dist = ceil (sqrt (best) * MSCALE);
      if (dist > 65535)
         dist = 65535;
      fix[bestn].dist = dist;
      fix[bestn].keep = 1;      // keep this middle point (used to find the next block to process)
      h = bestn;                // First half recursive
   }
   if (fixdebug)
      revk_info (TAG, "RDP completed in %uus", (unsigned int) (esp_timer_get_time () - start));
   // Sort
   unsigned int lost = 0,
      kept = 0;
   if (H > 2)
   {
      qsort (fix + 1, H - 1, sizeof (*fix), fixdistcmp);
      int n = H - 1;
      if (n > max)
         n = max;
      lost = fix[n].dist;       // Largest lost
      while (n > 1 && fix[n - 1].dist == lost)
         n--;                   // Same as largest lost, remove..
      kept = fix[n - 1].dist;   // Smallest kept
      if (n < H)
         fix[n] = fix[H];
      H = n;
      qsort (fix + 1, H - 1, sizeof (*fix), fixtimcmp);
   }
   if (dlostp)
      *dlostp = lost;
   if (dkeptp)
      *dkeptp = kept;
   return H;
}

void
gps_task (void *z)
{
   while (1)
   {                            // main task
      sleep (1);
      if (gpsstarted < 0)
         gps_init ();
      fixcheck (0);
      if (gpszda && fixsave >= 0)
      {                         // Time to save a fix
         fixnow = 0;
         // Reduce fixes
         int last = 0;
         if (fixsave > 0)
            last = fixsave - 1;
         unsigned int dlost = 0,
            dkept = 0;
         xSemaphoreTake (track_mutex, portMAX_DELAY);
         if (tracki >= MAXTRACK && tracko < tracki + 1 - MAXTRACK)
            tracko = tracki + 1 - MAXTRACK;     // Lost as wrapped
         tracklen[tracki % MAXTRACK] = 0;       // Ensure not sent until we have put in data
         xSemaphoreGive (track_mutex);
         uint8_t *t = track[tracki % MAXTRACK],
            *p = t + 8;
         if (fixend > fixbase)
         {
            *p++ = TAGF_PERIOD; // Time covered
            *p++ = (fixend - fixbase) >> 8;
            *p++ = (fixend - fixbase);
         }
         if (last && balloon)
            *p++ = TAGF_BALLOON;        // Alt scale flags
         else if (last && flight)
            *p++ = TAGF_FLIGHT;
         if (!tracki)
         {                      // First message
            *p++ = TAGF_FIRST;
            *p++ = fixbase >> 24;
            *p++ = fixbase >> 16;
            *p++ = fixbase >> 8;
            *p++ = fixbase;
         }
         if (datatemp)
         {
            int t = tempc * CSCALE;
            if (t > -128 && t < 127)
            {
               *p++ = TAGF_TEMPC;
               *p++ = t;
            }
         }
         uint8_t fixtag = TAGF_FIX | datafix;
         unsigned int fixlen = 10;
         for (int n = 0; n < sizeof (tagf_fix); n++)
            if (fixtag & (1 << n))
               fixlen += tagf_fix[n];
         uint8_t *q = p;
         if (datamargin)
            q += 3;
         q += 2;                // fix tag to be added once we know it
         unsigned int max = (MAXDATA - 16 - (q - t)) / fixlen;  // How many fixes we can fit...
         last = rdp (last, max, &dlost, &dkept);
         if (fixdebug)
            revk_info (TAG, "Logging %u/%u from %u fixes at %u." MPART "/%u." MPART "/%u." MPART " covering %u seconds", last,
                       max, fixsave, fix[1].dist / MSCALE, fix[1].dist % MSCALE, dkept / MSCALE, dkept % MSCALE, dlost / MSCALE,
                       dlost % MSCALE, fixend - fixbase);
         if (last > max)
            last = max;         // truncate
         if (last)
         {
            if (datamargin)
            {
               *p++ = TAGF_MARGIN;
               *p++ = dlost >> 8;       // Max distance of deleted points
               *p++ = dlost;
            }
            while (((p + 2 + last * fixlen - t) & 0xF) != 8)
               *p++ = TAGF_PAD; // Pre pad for fix
            *p++ = fixtag;      // Tag for fixes
            *p++ = fixlen;
            for (int n = 1; n <= last; n++)
            {                   // Don't send first as it is duplicate of last from previous packet
               fix_t *f = &fix[n];
               // Base fix data
               unsigned int v = f->tim; // Time
               *p++ = v >> 8;
               *p++ = v;
               v = f->lat;      // Lat
               *p++ = v >> 24;
               *p++ = v >> 16;
               *p++ = v >> 8;
               *p++ = v;
               v = f->lon;      // Lon
               *p++ = v >> 24;
               *p++ = v >> 16;
               *p++ = v >> 8;
               *p++ = v;
               // Optional fix data
               if (fixtag & TAGF_FIX_ALT)
               {
                  if (!f->alt)
                  {             // No alt
                     *p++ = 0x80;
                     *p++ = 0;
                  } else
                  {
                     v = (int) f->alt - (int) (ALTBASE / ascale);       // Alt
                     if (v > 32767)
                        v = 32767;      // Fit to signed 16 bits
                     *p++ = v >> 8;
                     *p++ = v;
                  }
               }
               if (fixtag & TAGF_FIX_SATS)
                  *p++ = (f->sats & 0x3F) + (f->dgps ? 0x80 : 0);
               if (fixtag & TAGF_FIX_HEPE)
                  *p++ = f->hepe;
            }
         }
         unsigned int len = encode (t, p - t, fixbase);
         if (len)
         {
            xSemaphoreTake (track_mutex, portMAX_DELAY);
            tracklen[tracki % MAXTRACK] = len;
            tracki++;
            xSemaphoreGive (track_mutex);
         }
         if (fixsave == 1)
            fixdelete = 1;      // Delete the one entry and start with no fixes
         else if (fixsave)
            fixdelete = fixsave - 1;
         else
            fixdelete = 0;
         fixsave = -1;
      }
   }
}

void
ds18b20_task (void *z)
{                               // temperature
   z = z;
   while (1)
   {
      usleep (100000);
      ds18b20_convert_all (owb);
      ds18b20_wait_for_conversion (ds18b20s[0]);
      float readings[MAX_OWB] = { 0 };
      DS18B20_ERROR errors[MAX_OWB] = { 0 };
      for (int i = 0; i < num_owb; ++i)
         errors[i] = ds18b20_read_temp (ds18b20s[i], &readings[i]);
      if (!errors[0])
         tempc = readings[0];
   }
}

void
app_main ()
{
   esp_err_t err;
   cmd_mutex = xSemaphoreCreateMutex ();        // Shared command access
   vSemaphoreCreateBinary (ack_semaphore);      // GPS ACK mutex
   at_mutex = xSemaphoreCreateMutex (); // Shared command access
   track_mutex = xSemaphoreCreateMutex ();
   revk_init (&app_command);
#define b(n,d) revk_register(#n,0,sizeof(n),&n,#d,SETTING_BOOLEAN);
#define bl(n,d) revk_register(#n,0,sizeof(n),&n,#d,SETTING_BOOLEAN|SETTING_LIVE);
#define h(n) revk_register(#n,0,0,&n,NULL,SETTING_BINARY|SETTING_HEX);
#define u32(n,d) revk_register(#n,0,sizeof(n),&n,#d,0);
#define u16(n,d) revk_register(#n,0,sizeof(n),&n,#d,0);
#define s8(n,d) revk_register(#n,0,sizeof(n),&n,#d,SETTING_SIGNED);
#define u8(n,d) revk_register(#n,0,sizeof(n),&n,#d,0);
#define s(n,d) revk_register(#n,0,0,&n,d,0);
   settings;
#undef u16
#undef u32
#undef s8
#undef u8
#undef bl
#undef b
#undef h
#undef s
   if (balloon)
      ascale = ALT_BALLOON;
   else if (flight)
      ascale = ALT_FLIGHT;
   if (mtu > 1488)
      mtu = 1488;
   // Memory
   track = heap_caps_malloc (sizeof (*track) * MAXTRACK, MALLOC_CAP_SPIRAM);
   if (!track)
      track = malloc (sizeof (*track) * (MAXTRACK = MAXTRACKLOW));
   if (!track)
   {
      revk_error ("malloc", "track failed");
      return;
   }
   tracklen = heap_caps_malloc (sizeof (*tracklen) * MAXTRACK, MALLOC_CAP_SPIRAM) ? : malloc (sizeof (*tracklen) * MAXTRACK);
   if (!tracklen)
   {
      revk_error ("malloc", "tracklen failed");
      return;
   }
   memset (tracklen, 0, sizeof (*tracklen) * MAXTRACK);
   for (int n = 0; n < MAXTRACK; n++)
   {
      track[n] = heap_caps_malloc (MAXDATA, MALLOC_CAP_SPIRAM) ? : malloc (MAXDATA);
      if (!track[n])
      {
         revk_error ("malloc", "track[%d] failed", n);
         return;
      }
   }
   fix = heap_caps_malloc (sizeof (*fix) * MAXFIX, MALLOC_CAP_SPIRAM);
   if (!fix)
      fix = malloc (sizeof (*fix) * (MAXFIX = MAXFIXLOW));
   if (!fix)
   {
      revk_error ("malloc", "fix failed");
      return;
   }
   if (oledsda >= 0 && oledscl >= 0)
      oled_start (1, oledaddress, oledscl, oledsda, oledflip);
   oled_set_contrast (oledcontrast);
   for (int x = 0; x < CONFIG_OLED_WIDTH; x++)
   {
      oled_set (x, CONFIG_OLED_HEIGHT - 12, 4);
      oled_set (x, CONFIG_OLED_HEIGHT - 12 - 3 - 24, 4);
      oled_set (x, CONFIG_OLED_HEIGHT - 12 - 3 - 24 - 3 - 22, 4);
      oled_set (x, 8, 4);
   }
   if (oledsda >= 0 && oledscl >= 0)
      revk_task ("Display", display_task, NULL);
   // Main task...
   if (gpspps >= 0)
      gpio_set_direction (gpspps, GPIO_MODE_INPUT);
   if (gpsfix >= 0)
      gpio_set_direction (gpsfix, GPIO_MODE_INPUT);
   if (gpspps >= 0)
      gpio_set_direction (gpspps, GPIO_MODE_INPUT);
   if (gpsen >= 0)
   {                            // Enable
      gpio_set_level (gpsen, 1);
      gpio_set_direction (gpsen, GPIO_MODE_OUTPUT);
   }
   {
      // Init UART for GPS
      void connect (unsigned int baud)
      {
         uart_config_t uart_config = {
            .baud_rate = baud,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
         };
         if ((err = uart_param_config (gpsuart, &uart_config)))
            revk_error (TAG, "UART param fail %s", esp_err_to_name (err));
         else if ((err = uart_set_pin (gpsuart, gpstx, gpsrx, -1, -1)))
            revk_error (TAG, "UART pin fail %s", esp_err_to_name (err));
         else if ((err = uart_driver_install (gpsuart, 1024, 0, 0, NULL, 0)))
            revk_error (TAG, "UART install fail %s", esp_err_to_name (err));
      }
      connect (gpsbaud);
      uint8_t temp;
      if (gpsbaud != 9600 && (uart_read_bytes (gpsuart, &temp, 1, 1000 / portTICK_PERIOD_MS) <= 0 || temp != '$'))
      {
         uart_driver_delete (gpsuart);
         connect (9600);
         sleep (1);
         gpscmd ("$PMTK251,%d", gpsbaud);       // Baud rate set (at 9600)
         sleep (1);
         uart_driver_delete (gpsuart);
         connect (gpsbaud);
      }
   }
   if (attx >= 0 && atrx >= 0 && atpwr >= 0)
   {
      // Init UART for Mobile
      uart_config_t uart_config = {
         .baud_rate = atbaud,
         .data_bits = UART_DATA_8_BITS,
         .parity = UART_PARITY_DISABLE,
         .stop_bits = UART_STOP_BITS_1,
         .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      };
      if ((err = uart_param_config (atuart, &uart_config)))
         revk_error (TAG, "UART param fail %s", esp_err_to_name (err));
      else if ((err = uart_set_pin (atuart, attx, atrx, -1, -1)))
         revk_error (TAG, "UART pin fail %s", esp_err_to_name (err));
      else if ((err = uart_driver_install (atuart, 1024, 0, 0, NULL, 0)))
         revk_error (TAG, "UART install fail %s", esp_err_to_name (err));
      if (atkey >= 0)
         gpio_set_direction (atkey, GPIO_MODE_OUTPUT);
      if (atrst >= 0)
         gpio_set_direction (atrst, GPIO_MODE_OUTPUT);
      if (atpwr >= 0)
         gpio_set_direction (atpwr, GPIO_MODE_OUTPUT);
      revk_task ("Mobile", at_task, NULL);
   }
   if (light >= 0)
      gpio_set_direction (light, GPIO_MODE_OUTPUT);
   if (gpsrx >= 0)
      revk_task ("NMEA", nmea_task, NULL);
   revk_task ("Log", log_task, NULL);
   revk_task ("GPS", gps_task, NULL);
   if (ds18b20 >= 0)
   {                            // DS18B20 init
      owb = owb_rmt_initialize (&rmt_driver_info, ds18b20, RMT_CHANNEL_1, RMT_CHANNEL_0);
      owb_use_crc (owb, true);  // enable CRC check for ROM code
      OneWireBus_ROMCode device_rom_codes[MAX_OWB] = { 0 };
      OneWireBus_SearchState search_state = { 0 };
      bool found = false;
      owb_search_first (owb, &search_state, &found);
      while (found && num_owb < MAX_OWB)
      {
         char rom_code_s[17];
         owb_string_from_rom_code (search_state.rom_code, rom_code_s, sizeof (rom_code_s));
         device_rom_codes[num_owb] = search_state.rom_code;
         ++num_owb;
         owb_search_next (owb, &search_state, &found);
      }
      for (int i = 0; i < num_owb; i++)
      {
         DS18B20_Info *ds18b20_info = ds18b20_malloc ();        // heap allocation
         ds18b20s[i] = ds18b20_info;
         if (num_owb == 1)
            ds18b20_init_solo (ds18b20_info, owb);      // only one device on bus
         else
            ds18b20_init (ds18b20_info, owb, device_rom_codes[i]);      // associate with bus and device
         ds18b20_use_crc (ds18b20_info, true);  // enable CRC check for temperature readings
         ds18b20_set_resolution (ds18b20_info, DS18B20_RESOLUTION);
      }
      if (!num_owb)
         revk_error ("temp", "No OWB devices");
      else
         revk_task ("DS18B20", ds18b20_task, NULL);
   }
}
