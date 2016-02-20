
/*
  dropsonde RD94
  frames,position: 2Hz
  velocity(wind): 4Hz
*/

#include <stdio.h>
#include <string.h>
#include <math.h>


typedef unsigned char  ui8_t;
typedef unsigned short ui16_t;
typedef unsigned int   ui32_t;

int option_verbose = 0,  // ausfuehrliche Anzeige
    option_raw = 0,      // rohe Frames
    option_inv = 0,      // invertiert Signal
    fileloaded = 0,
    option_res = 0,
    rawin = 0;

typedef struct {
    int frnr1;
    int frnr2;
    char id[9];
    int week; int gpssec;
    int jahr; int monat; int tag;
    int wday;
    int std; int min; int sek; int ms;
    double lat; double lon; double h;
    double vN; double vE; double vU;
    double vH; double vD; double vD2;
} gpx_t;

gpx_t gpx;

#define BITS (1+8+1)  // 8N1 = 10bit/byte

#define HEADLEN (60)
#define HEADOFS (40)

char header[] = 
"10100110010110101001"  // 0x1A = 0 01011000 1
"10010101011010010101"  // 0xCF = 0 11110011 1
"10101001010101010101"  // 0xFC = 0 00111111 1
"10011001010110101001"  // 0x1D = 0 10111000 1
"10011010101010101001"; // 0x01 = 0 10000000 1

char buf[HEADLEN+1] = "x";
int bufpos = -1;

#define       FRAME_LEN  120  // 240/sec -> 120/frame
#define    BITFRAME_LEN (FRAME_LEN*BITS)
#define RAWBITFRAME_LEN (FRAME_LEN*BITS*2)

char frame_rawbits[RAWBITFRAME_LEN+8];
char frame_bits[BITFRAME_LEN+4];
ui8_t frame_bytes[FRAME_LEN+10];


/* ------------------------------------------------------------------------------------ */

#define BAUD_RATE 4800

int sample_rate = 0, bits_sample = 0, channels = 0;
float samples_per_bit = 0;

int findstr(char *buf, char *str, int pos) {
    int i;
    for (i = 0; i < 4; i++) {
        if (buf[(pos+i)%4] != str[i]) break;
    }
    return i;
}

int read_wav_header(FILE *fp) {
    char txt[4+1] = "\0\0\0\0";
    unsigned char dat[4];
    int byte, p=0;

    if (fread(txt, 1, 4, fp) < 4) return -1;
    if (strncmp(txt, "RIFF", 4)) return -1;
    if (fread(txt, 1, 4, fp) < 4) return -1;
    // pos_WAVE = 8L
    if (fread(txt, 1, 4, fp) < 4) return -1;
    if (strncmp(txt, "WAVE", 4)) return -1;
    // pos_fmt = 12L
    for ( ; ; ) {
        if ( (byte=fgetc(fp)) == EOF ) return -1;
        txt[p % 4] = byte;
        p++; if (p==4) p=0;
        if (findstr(txt, "fmt ", p) == 4) break;
    }
    if (fread(dat, 1, 4, fp) < 4) return -1;
    if (fread(dat, 1, 2, fp) < 2) return -1;

    if (fread(dat, 1, 2, fp) < 2) return -1;
    channels = dat[0] + (dat[1] << 8);

    if (fread(dat, 1, 4, fp) < 4) return -1;
    memcpy(&sample_rate, dat, 4); //sample_rate = dat[0]|(dat[1]<<8)|(dat[2]<<16)|(dat[3]<<24);

    if (fread(dat, 1, 4, fp) < 4) return -1;
    if (fread(dat, 1, 2, fp) < 2) return -1;
    //byte = dat[0] + (dat[1] << 8);

    if (fread(dat, 1, 2, fp) < 2) return -1;
    bits_sample = dat[0] + (dat[1] << 8);

    // pos_dat = 36L + info
    for ( ; ; ) {
        if ( (byte=fgetc(fp)) == EOF ) return -1;
        txt[p % 4] = byte;
        p++; if (p==4) p=0;
        if (findstr(txt, "data", p) == 4) break;
    }
    if (fread(dat, 1, 4, fp) < 4) return -1;


    fprintf(stderr, "sample_rate: %d\n", sample_rate);
    fprintf(stderr, "bits       : %d\n", bits_sample);
    fprintf(stderr, "channels   : %d\n", channels);

    if ((bits_sample != 8) && (bits_sample != 16)) return -1;

    samples_per_bit = sample_rate/(float)BAUD_RATE;

    fprintf(stderr, "samples/bit: %.2f\n", samples_per_bit);

    return 0;
}


#define EOF_INT  0x1000000

int read_signed_sample(FILE *fp) {  // int = i32_t
    int byte, i, ret;         //  EOF -> 0x1000000

    for (i = 0; i < channels; i++) {
                           // i = 0: links bzw. mono
        byte = fgetc(fp);
        if (byte == EOF) return EOF_INT;
        if (i == 0) ret = byte;
    
        if (bits_sample == 16) {
            byte = fgetc(fp);
            if (byte == EOF) return EOF_INT;
            if (i == 0) ret +=  byte << 8;
        }

    }

    if (bits_sample ==  8) return ret-128;   // 8bit: 00..FF, centerpoint 0x80=128
    if (bits_sample == 16) return (short)ret;

    return ret;
}

int par=1, par_alt=1;
unsigned long sample_count = 0;

int read_bits_fsk(FILE *fp, int *bit, int *len) {
    static int sample;
    int n, y0;
    float l, x1;
    static float x0;

    n = 0;
    do {
        y0 = sample;
        sample = read_signed_sample(fp);
        if (sample == EOF_INT) return EOF;
        sample_count++;
        par_alt = par;
        par =  (sample >= 0) ? 1 : -1;    // 8bit: 0..127,128..255 (-128..-1,0..127)
        n++;
    } while (par*par_alt > 0);

    if (!option_res) l = (float)n / samples_per_bit;
    else {                                 // genauere Bitlaengen-Messung
        x1 = sample/(float)(sample-y0);    // hilft bei niedriger sample rate
        l = (n+x0-x1) / samples_per_bit;   // meist mehr frames (nicht immer)
        x0 = x1;
    }

    *len = (int)(l+0.5);

    if (!option_inv) *bit = (1+par_alt)/2;  // oben 1, unten -1
    else             *bit = (1-par_alt)/2;  // sdr#<rev1381?, invers: unten 1, oben -1
// *bit = (1+inv*par_alt)/2; // ausser inv=0

    return 0;
}

/* ------------------------------------------------------------------------------------ */
/*
 * Convert GPS Week and Seconds to Modified Julian Day.
 * - Adapted from sci.astro FAQ.
 * - Ignores UTC leap seconds.
 */
void Gps2Date(long GpsWeek, long GpsSeconds, int *Year, int *Month, int *Day) {

    long GpsDays, Mjd;
    long J, C, Y, M;

    GpsDays = GpsWeek * 7 + (GpsSeconds / 86400);
    Mjd = 44244 + GpsDays;

    J = Mjd + 2468570;
    C = 4 * J / 146097;
    J = J - (146097 * C + 3) / 4;
    Y = 4000 * (J + 1) / 1461001;
    J = J - 1461 * Y / 4 + 31;
    M = 80 * J / 2447;
    *Day = J - 2447 * M / 80;
    J = M / 11;
    *Month = M + 2 - (12 * J);
    *Year = 100 * (C - 49) + Y + J;
}
/* ------------------------------------------------------------------------------------ */


#define OFS           (0x03)  // HEADLEN/(2*BITS)
#define pos_FrameNb   (OFS+0x00)   // 2 byte
#define pos_GPSTOW    (OFS+0x17)   // 4 byte
#define pos_GPSweek   (OFS+0x1F)   // 2 byte
#define pos_GPSecefX  (OFS+0x23)   // 4 byte
#define pos_GPSecefY  (OFS+0x27)   // 4 byte
#define pos_GPSecefZ  (OFS+0x2B)   // 4 byte
#define pos_GPSV      (OFS+0x2F)   // 4 byte...
#define pos_GPSecefV1 (OFS+0x33)   // 3*4 byte...
#define pos_GPSecefV2 (OFS+0x49)   // 3*4 byte...


int get_FrameNb() {
    int i;
    unsigned byte;
    ui8_t frnr_bytes[4];
    int frnr;

    for (i = 0; i < 2; i++) {
        byte = frame_bytes[pos_FrameNb + i];
        frnr_bytes[i] = byte;
    }
    frnr = frnr_bytes[0] + (frnr_bytes[1] << 8);
    gpx.frnr1 = frnr;

    for (i = 0; i < 4; i++) {
        byte = frame_bytes[pos_FrameNb+2 + i];
        frnr_bytes[i] = byte;
    }
    gpx.frnr2 = frnr_bytes[0] | (frnr_bytes[1] << 8) | (frnr_bytes[2] << 16) | (frnr_bytes[3] << 24);

    return 0;
}

int get_GPSweek() {
    int i;
    unsigned byte;
    ui8_t gpsweek_bytes[2];
    int gpsweek;

    for (i = 0; i < 2; i++) {
        byte = frame_bytes[pos_GPSweek + i];
        gpsweek_bytes[i] = byte;
    }

    gpsweek = gpsweek_bytes[0] + (gpsweek_bytes[1] << 8);
    if (gpsweek < 0) { gpx.week = -1; return -1; }
    gpx.week = gpsweek;

    return 0;
}

char weekday[7][3] = { "So", "Mo", "Di", "Mi", "Do", "Fr", "Sa"};

int get_GPStime() {
    int i;
    unsigned byte;
    ui8_t gpstime_bytes[4];
    int gpstime = 0, // 32bit
        day;

    for (i = 0; i < 4; i++) {
        byte = frame_bytes[pos_GPSTOW + i];
        gpstime_bytes[i] = byte;
    }

    memcpy(&gpstime, gpstime_bytes, 4);
    gpx.ms = gpstime % 1000;
    gpstime /= 1000;

    gpx.gpssec = gpstime;

    day = gpstime / (24 * 3600);
    gpstime %= (24*3600);

    if ((day < 0) || (day > 6)) return -1;
    gpx.wday = day;
    gpx.std = gpstime / 3600;
    gpx.min = (gpstime % 3600) / 60;
    gpx.sek = gpstime % 60;

    return 0;
}

#define EARTH_a  6378137.0
#define EARTH_b  6356752.31424518
#define EARTH_a2_b2  (EARTH_a*EARTH_a - EARTH_b*EARTH_b)

double a = EARTH_a,
       b = EARTH_b,
       a_b = EARTH_a2_b2,
       e2  = EARTH_a2_b2 / (EARTH_a*EARTH_a),
       ee2 = EARTH_a2_b2 / (EARTH_b*EARTH_b);

void ecef2elli(double X[], double *lat, double *lon, double *h) {
    double phi, lam, R, p, t;

    lam = atan2( X[1] , X[0] );

    p = sqrt( X[0]*X[0] + X[1]*X[1] );
    t = atan2( X[2]*a , p*b );
    
    phi = atan2( X[2] + ee2 * b * sin(t)*sin(t)*sin(t) ,
                 p - e2 * a * cos(t)*cos(t)*cos(t) );

    R = a / sqrt( 1 - e2*sin(phi)*sin(phi) );
    *h = p / cos(phi) - R;
    
    *lat = phi*180/M_PI;
    *lon = lam*180/M_PI;
}


int get_GPSkoord() {
    int i, k;
    unsigned byte;
    ui8_t XYZ_bytes[4];
    int XYZ; // 32bit
    double X[3], lat, lon, h;
/*
    ui8_t gpsVel_bytes[4];
    int vel32; // 32bit
    double V[3], phi, lam, alpha, dir;
*/

    for (k = 0; k < 3; k++) {

        for (i = 0; i < 4; i++) {
            byte = frame_bytes[pos_GPSecefX + 4*k + i];
            XYZ_bytes[i] = byte;
        }
        memcpy(&XYZ, XYZ_bytes, 4);
        X[k] = XYZ / 100.0;
/*
        for (i = 0; i < 4; i++) {
            frame[pos_GPSecefV + 2*k + i];
            byte = byte ^ mask[(pos_GPSecefV + 2*k + i) % MASK_LEN];
            gpsVel_bytes[i] = byte;
        }
        vel32 = gpsVel_bytes[0] | gpsVel_bytes[1] << 8 | gpsVel_bytes[2] << 16 | gpsVel_bytes[3] << 24;
        V[k] = vel32 / 100.0;
*/
    }


    // ECEF-Position
    ecef2elli(X, &lat, &lon, &h);
    gpx.lat = lat;
    gpx.lon = lon;
    gpx.h = h;
    if ((h < -1000) || (h > 80000)) return -1;

/*
    // ECEF-Velocities
    // ECEF-Vel -> NorthEastUp
    phi = lat*M_PI/180.0;
    lam = lon*M_PI/180.0;
    gpx.vN = -V[0]*sin(phi)*cos(lam) - V[1]*sin(phi)*sin(lam) + V[2]*cos(phi);
    gpx.vE = -V[0]*sin(lam) + V[1]*cos(lam);
    gpx.vU =  V[0]*cos(phi)*cos(lam) + V[1]*cos(phi)*sin(lam) + V[2]*sin(phi);

    // NEU -> HorDirVer
    gpx.vH = sqrt(gpx.vN*gpx.vN+gpx.vE*gpx.vE);
//
    alpha = atan2(gpx.vN, gpx.vE)*180/M_PI;  // ComplexPlane (von x-Achse nach links) - GeoMeteo (von y-Achse nach rechts)
    dir = 90-alpha;                          // z=x+iy= -> i*conj(z)=y+ix=re(i(pi/2-t)), Achsen und Drehsinn vertauscht
    if (dir < 0) dir += 360;                 // atan2(y,x)=atan(y/x)=pi/2-atan(x/y) , atan(1/t) = pi/2 - atan(t)
    gpx.vD2 = dir;
//
    dir = atan2(gpx.vE, gpx.vN) * 180 / M_PI;
    if (dir < 0) dir += 360;
    gpx.vD = dir;
*/
    return 0;
}

int get_V() {
    int i, k;
    unsigned byte;
    ui8_t XYZ_bytes[4];
    int XYZ; // 32bit
    double X, V[3];
    double phi, lam, dir, vD, vH, vN, vE, vU;


    for (i = 0; i < 4; i++) {
        byte = frame_bytes[pos_GPSV + i];
        XYZ_bytes[i] = byte;
    }
    memcpy(&XYZ, XYZ_bytes, 4);
    X = XYZ / 100.0;
    if (option_verbose) {
        printf(" # ");
        printf(" %6.2f ", X);
    }

    for (k = 0; k < 3; k++) {
        for (i = 0; i < 4; i++) {
            byte = frame_bytes[pos_GPSecefV1 + 4*k + i];
            XYZ_bytes[i] = byte;
        }
        memcpy(&XYZ, XYZ_bytes, 4);
        V[k] = XYZ / 100.0;
    }
    if (option_verbose) {
        printf(" # ");
        printf(" (%7.2f,%7.2f,%7.2f) ", V[0], V[1], V[2]);
    }

    phi = gpx.lat*M_PI/180.0;
    lam = gpx.lon*M_PI/180.0;
    vN = -V[0]*sin(phi)*cos(lam) - V[1]*sin(phi)*sin(lam) + V[2]*cos(phi);
    vE = -V[0]*sin(lam) + V[1]*cos(lam);
    vU =  V[0]*cos(phi)*cos(lam) + V[1]*cos(phi)*sin(lam) + V[2]*sin(phi);

    vH = sqrt(vN*vN+vE*vE);
    dir = atan2(vE, vN) * 180 / M_PI;
    if (dir < 0) dir += 360;
    vD = dir;
    fprintf(stdout,"  vH: %.1fm/s  D: %.1f°  vV: %.1fm/s ", vH, vD, vU);

    for (k = 0; k < 3; k++) {
        for (i = 0; i < 4; i++) {
            byte = frame_bytes[pos_GPSecefV2 + 4*k + i];
            XYZ_bytes[i] = byte;
        }
        memcpy(&XYZ, XYZ_bytes, 4);
        V[k] = XYZ / 100.0;
    }
    if (option_verbose) {
    //printf(" # ");
        printf(" (%7.2f,%7.2f,%7.2f) ", V[0], V[1], V[2]);
    }

    return 0;
}

/* ------------------------------------------------------------------------------------ */


void inc_bufpos() {
  bufpos = (bufpos+1) % HEADLEN;
}

int compare() {
    int i, j;

    i = 0;
    j = bufpos;
    while (i < HEADLEN) {
        if (j < 0) j = HEADLEN-1;
        if (buf[j] != header[HEADOFS+HEADLEN-1-i]) break;
        j--;
        i++;
    }
    if (i == HEADLEN) return 1;
/*
    i = 0;
    j = bufpos;
    while (i < HEADLEN) {
        if (j < 0) j = HEADLEN-1;
        if (buf[j] != cb_inv(header[HEADOFS+HEADLEN-1-i])) break;
        j--;
        i++;
    }
    if (i == HEADLEN) return -1;
*/
    return 0;

}

// manchester1 1->10,0->01: 1.bit
// manchester2 0->10,1->01: 2.bit
void manchester2(char* frame_rawbits, char *frame_bits) {
    int i;
    char bit, bits[2];

    for (i = 0; i < BITFRAME_LEN; i++) {
        bits[0] = frame_rawbits[2*i];
        bits[1] = frame_rawbits[2*i+1];

        if ((bits[0] == '0') && (bits[1] == '1')) bit = '1';
        else
        if ((bits[0] == '1') && (bits[1] == '0')) bit = '0';
        else bit = 'x';
        frame_bits[i] = bit;
    }
}

int bits2bytes(char *bitstr, ui8_t *bytes) {
    int i, bit, d, byteval;
    int bitpos, bytepos;

    bitpos = 0;
    bytepos = 0;

    while (bytepos < FRAME_LEN) {

        byteval = 0;
        d = 1;
        for (i = 1; i < BITS-1; i++) { 
            bit=*(bitstr+bitpos+i); /* little endian */
            //bit=*(bitstr+bitpos+BITS-1-i);  /* big endian */
            if         (bit == '1')     byteval += d;
            else /*if ((bit == '0') */  byteval += 0;
            d <<= 1;
        }
        bitpos += BITS;
        bytes[bytepos++] = byteval;
        
    }

    //while (bytepos < FRAME_LEN) bytes[bytepos++] = 0;

    return 0;
}


void print_frame(int len) {
    int i, err;

    for (i = len; i < RAWBITFRAME_LEN; i++) frame_rawbits[i] = '0';
    manchester2(frame_rawbits, frame_bits);
    bits2bytes(frame_bits, frame_bytes);


    if (option_raw == 1) {
        for (i = 0; i < BITFRAME_LEN; i++) {
            fprintf(stdout, "%c", frame_bits[i]);
        }
        fprintf(stdout, "\n");
/*
        for (i = 0; i < RAWBITFRAME_LEN; i++) {
            fprintf(stdout, "%c", frame_rawbits[i]);
        }
        fprintf(stdout, "\n");
*/
    }
    else if (option_raw == 2) {
        for (i = 0; i < FRAME_LEN; i++) {
            //fprintf(stdout, "%02x", frame_bytes[i]);
            fprintf(stdout, "%02X ", frame_bytes[i]);
        }
        fprintf(stdout, "\n");
    }
    else {

        err = 0;
        err |= get_FrameNb();
        //err |= get_SondeID();
        err |= get_GPSweek();
        err |= get_GPStime();
        err |= get_GPSkoord();
        if (!err) {
            Gps2Date(gpx.week, gpx.gpssec, &gpx.jahr, &gpx.monat, &gpx.tag);
            fprintf(stdout, "[%5d]  ", gpx.frnr1);
            //fprintf(stdout, "0x%08X ", gpx.frnr2);
            //fprintf(stdout, "(%s) ", gpx.id);
            fprintf(stdout, "%s ", weekday[gpx.wday]);
            fprintf(stdout, "%04d-%02d-%02d %02d:%02d:%02d.%03d", 
                    gpx.jahr, gpx.monat, gpx.tag, gpx.std, gpx.min, gpx.sek, gpx.ms);
            if (option_verbose) fprintf(stdout, " (W %d)", gpx.week);
            fprintf(stdout, "  ");
            fprintf(stdout, " lat: %.5f° ", gpx.lat);
            fprintf(stdout, " lon: %.5f° ", gpx.lon);
            fprintf(stdout, " alt: %.2fm ", gpx.h);
/*
            if (option_verbose) {
                //fprintf(stdout, "  (%.1f %.1f %.1f) ", gpx.vN, gpx.vE, gpx.vU);
                fprintf(stdout,"  vH: %.1f  D: %.1f°  vV: %.1f ", gpx.vH, gpx.vD, gpx.vU);
            }
*/
            if (len > 2*BITS*(pos_GPSecefV2+12))  get_V();

            fprintf(stdout, "\n");  // fflush(stdout);
        }
    }
}


int main(int argc, char **argv) {

    FILE *fp = NULL;
    char *fpname;
    char *pbuf = NULL;
    int header_found = 0;
    int i, pos, bit, len;

    fpname = argv[0];
    ++argv;
    while ((*argv) && (!fileloaded)) {
        if      ( (strcmp(*argv, "-h") == 0) || (strcmp(*argv, "--help") == 0) ) {
            fprintf(stderr, "%s [options] <file>\n", fpname);
            fprintf(stderr, "  file: audio.wav or raw_data\n");
            fprintf(stderr, "  options:\n");
            fprintf(stderr, "       -v, --verbose\n");
            fprintf(stderr, "       -r, --rawbits\n");
            fprintf(stderr, "       -R, --rawbytes\n");
            fprintf(stderr, "       -i, --invert\n");
            fprintf(stderr, "       --rawin  (rawbits file)\n");
            return 0;
        }
        else if ( (strcmp(*argv, "-v") == 0) || (strcmp(*argv, "--verbose") == 0) ) {
            option_verbose = 1;
        }
        else if ( (strcmp(*argv, "-r") == 0) || (strcmp(*argv, "--rawbits") == 0) ) {
            option_raw = 1;
        }
        else if ( (strcmp(*argv, "-R") == 0) || (strcmp(*argv, "--rawbytes") == 0) ) {
            option_raw = 2;
        }
        else if ( (strcmp(*argv, "-i") == 0) || (strcmp(*argv, "--invert") == 0) ) {
            option_inv = 1;
        }
        else if (strcmp(*argv, "--rawin") == 0) { rawin = 1; }     // rawbits input
        else {
            if (!rawin) fp = fopen(*argv, "rb");
            else        fp = fopen(*argv, "r");
            if (fp == NULL) {
                fprintf(stderr, "%s konnte nicht geoeffnet werden\n", *argv);
                return -1;
            }
            fileloaded = 1;
        }
        ++argv;
    }
    if (!fileloaded) fp = stdin;


    if (!rawin) {

        i = read_wav_header(fp);
        if (i) {
            fclose(fp);
            return -1;
        }

        for (pos = 0; pos < HEADLEN; pos++) {
            frame_rawbits[pos] = header[HEADOFS+pos];
        }

        while (!read_bits_fsk(fp, &bit, &len)) {

            if (len == 0) { // reset_frame();
            /*  if (pos > 2*BITS*pos_GPSV) {
                    print_frame(pos);
                    pos = HEADLEN;
                    header_found = 0;
                } */
                continue;   // ...
            }

            for (i = 0; i < len; i++) {

                inc_bufpos();
                buf[bufpos] = 0x30+bit;

                if (!header_found) {
                    header_found = compare();
                }
                else {
                    frame_rawbits[pos] = 0x30+bit;
                    //printf("%d", bit);
                    pos++;
                    if (pos == RAWBITFRAME_LEN) {
                        //frames++;
                        print_frame(pos);
                        header_found = 0;
                        pos = HEADLEN;
                    }
                }
            }
        }

    }
    else {

        while (1 > 0) {
            pbuf = fgets(frame_rawbits, RAWBITFRAME_LEN+4, fp);
            if (pbuf == NULL) break;
            frame_rawbits[RAWBITFRAME_LEN+1] = '\0';
            len = strlen(frame_rawbits);
            if (len > 2*BITS*pos_GPSV) print_frame(len);
        }

    }

    fclose(fp);

    
    return 0;
}
