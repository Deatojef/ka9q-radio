// powerlogger
//
// This will log into the local database (postgresql), and save received power data as rows in that database.
//
// Copyright 2017-2024 Phil Karn, KA9Q & Jeff Deaton, N6BA
// Major revisions fall 2020, 2023, 2024 (really continuous revisions!)

#define _GNU_SOURCE 1
#include <assert.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <stdbool.h>
#include <limits.h>
#include <string.h>
#if defined(linux)
#include <bsd/string.h>
#include <bsd/stdlib.h> // for arc4random()
#endif
#include <math.h>
#include <complex.h>
#undef I
#include <poll.h>
#include <sys/time.h>
#include <sys/select.h>
#include <ncurses.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netdb.h>
#include <locale.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sysexits.h>
#include <errno.h>
#include <gps.h>

#include "misc.h"
#include "multicast.h"
#include "radio.h"
#include "config.h"
#include "db.h"


// Globals
static int const DEFAULT_IP_TOS = 48;
static int const DEFAULT_MCAST_TTL = 1; // LAN only, no routers
struct sockaddr_storage Metadata_dest_address;      // Dest of metadata (typically multicast)
int Mcast_ttl = DEFAULT_MCAST_TTL;
int IP_tos = DEFAULT_IP_TOS;
int Ctl_fd;
int Status_fd;

// defaults for command line arguments
int Verbose;
const char *App_path;
static float Refresh_rate = .25;
float SNR_threshold = 0.0;
bool stop_processing = false;
char *GPSDHost = "localhost";
char *Status_hostname = "2m.local";
char *pgpass_file = "/var/lib/ka9q-radio/.pgpass";
char *Logfilename = "/var/log/powerlogger.log";
char *dbname = "powerdata";
char *dbtable = "powerdata";
char *dbuser = "monitor";
char *dbhost = "localhost";
FILE *Logfile;


// structure to hold the SSRC data
typedef struct ssrcitem {
    int ssrc;
    int64_t lastpoll;
    struct channel Channel;
    struct frontend Frontend;
    uint64_t Block_drops; // Stored in output filter on sender, not in channel structure
    uint64_t Metadata_packets;
} ssrcitem_t;

// the list of ssrc items that we're tracking
ssrcitem_t *ssrclist;
int num_ssrc = 0;


// Thread mutex, conditions, and threads
// ---------------------------------------------------------------------
pthread_mutex_t gps_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_t gps_thread;
double gpslat = 0;
double gpslon = 0;
int gpsmode = 0;
void *gpsthread(void *arg);
// ---------------------------------------------------------------------


// functions
static int for_us(uint8_t const *buffer,int length);
static int init_demod(struct channel *channel);
void closedown(int x);
void publishdata(ssrcitem_t *ssrc);
ssrcitem_t *parse_int_string(const char *str, int *num_elements);
//static int decode_radio_status(ssrcitem_t *ssrcitem,uint8_t const *buffer,int length);
static int decode_status(ssrcitem_t *ssrcitem, uint8_t const *buffer, int length);
void rmch(char *source, char c, char *dest, size_t dest_size);
static int send_poll(int fd, int ssrc);


// Main routine that loops until killed.
int main(int argc,char *argv[])
{
    char timebuffer[1024];

    App_path = argv[0];
    {
        int c;
        while((c = getopt(argc,argv,"vVI:s:r:n:y:G:L:p:D:U:T:H:")) != -1) {
            switch(c) {
                case 'V':
                    VERSION();
                    exit(EXIT_SUCCESS);
                case 'v':
                    Verbose++;
                    break;
                case 's':
                    ssrclist = parse_int_string(optarg, &num_ssrc);
                    break;
                case 'r':
                    Refresh_rate = strtod(optarg,NULL);
                    break;
                case 'n':
                    SNR_threshold = strtod(optarg, NULL);
                    break;
                case 'G':
                    GPSDHost = optarg;
                    break;
                case 'L':
                    Logfilename = optarg;
                    break;
                case 'I':
                    Status_hostname = optarg;
                    break;
                case 'p':
                    pgpass_file = optarg;
                    break;
                case 'D':
                    dbname = optarg;
                    break;
                case 'U':
                    dbuser = optarg;
                    break;
                case 'T':
                    dbtable = optarg;
                    break;
                case 'H':
                    dbhost = optarg;
                    break;
                default:
                    fprintf(stdout,"Unknown option %c\n",c);
                    break;
            }
        }
    }

    // Check that proper arguments were used with starting up
    if (ssrclist == NULL || num_ssrc == 0 || !Status_hostname) {
        fprintf(stderr,"Usage: %s -s <ssrc list> -I <mcast_group> [-r <refresh rate in secs>] [-v] [-G GPSD hostname] [-n SNR threshold]\n",App_path);
        fprintf(stderr,"<ssrclist> is a list of space seperated frequences (ex. \"144m710 145m310\", mcast_group is DNS name or IP address of control multicast group, \n");
        exit(EX_USAGE);
    }

    // capture any sort of kill/close conditions.  Mostly so we can cleanly exit, disconnect from the database, etc..
    signal(SIGPIPE, closedown);
    signal(SIGINT, closedown);
    signal(SIGKILL, closedown);
    signal(SIGQUIT, closedown);
    signal(SIGTERM, closedown);

    // where are we logging output too?
    if (Logfilename) {
        Logfile = fopen(Logfilename, "a");
    }
    else if (Verbose) {
        Logfile = stdout;
    }

    if (Logfile) {
        setlinebuf(Logfile);
        format_gpstime(timebuffer, sizeof(timebuffer), gps_time_ns());
        fprintf(Logfile, "%s ################## START:  powerlogger ##############\n", timebuffer);


        // print out the list of frequencies we're listening too
        fprintf(Logfile, "%s Frequency List: ", timebuffer);
        int i;
        for (i = 0; i < num_ssrc; i++) {
            fprintf(Logfile, "%.3fMHz ", (float) ssrclist[i].ssrc / 1000.0);
        }
        if (i > 0)
            fprintf(Logfile, "\n");

        // other command line parameters
        fprintf(Logfile, "%s Multicast addr: %s, GPSD Host: %s, Refresh period (s): %.4ff, SNR Threshold: %.4f\n", timebuffer, Status_hostname, GPSDHost, Refresh_rate, SNR_threshold);

    }
    else {
        fprintf(stderr, "Unable to write to log file:  %s\n", Logfilename);
        exit(EX_IOERR);
    }

    // try and log into the database.  If unable to connect to the DB, then we can't proceed.
    if (db_init(dbhost, dbname, dbuser, dbtable, pgpass_file)) {
        if (Verbose)
            fprintf(Logfile, "%s Database connection successful\n", format_gpstime(timebuffer, sizeof(timebuffer), gps_time_ns()));
    } 
    else {
        fprintf(stderr, "Unable to connect to database\n");
        exit(EX_IOERR);
    }

    // This is the status RTP address (ex. 2m.local, 70cm.local)
    char iface[1024]; // Multicast interface
    resolve_mcast(Status_hostname, &Metadata_dest_address, DEFAULT_STAT_PORT, iface, sizeof(iface), 0);
    Status_fd = listen_mcast(&Metadata_dest_address, iface);
    
    // If unable to connect to the status multicast address then exit
    if(Status_fd == -1) {
        fprintf(stderr,"Can't listen to mcast status %s\n", Status_hostname);
        exit(EX_IOERR);
    }

    // this is the same status RTP address, except that we need to use this file descriptor to send a "poll" command to multicast group.
    Ctl_fd = connect_mcast(&Metadata_dest_address, iface, Mcast_ttl, IP_tos);
    if(Ctl_fd < 0) {
        fprintf(stderr,"connect to mcast control failed\n");
        exit(EX_IOERR);
    }



    /* Main loop:
       Send poll if we haven't received one in our refresh interval
       See if anything has arrived (use short timeout)
       If there's a response, update local status & repaint display windows
       Poll keyboard and process user commands

       Randomize polls over +/- 32 ms in case someone else is also polling
       This avoids possible synchronized back-to-back polls
       This is a common technique in multicast protocols (e.g., IGMP queries)
    */

    int const random_interval = 64 << 20; // power of 2 makes it easier for arc4random_uniform()
    int64_t now = gps_time_ns();
    int64_t next_radio_poll = now; // Immediate first poll

    // print out the header
    if (Verbose)
        fprintf(stdout, "timestamp, gps_fix_mode, gps_lat, gps_lon, ssrc, lna_gain, mixer_gain, if_gain, input_power_dbfs, frontend_gain_db, baseband_power_db, snr_dB\n");

    // start the GPS client thread
    pthread_create(&gps_thread,NULL,gpsthread,NULL);

    // Loop forever
    while(!stop_processing) {

        int64_t const radio_poll_interval = Refresh_rate * BILLION; // Can change from the keyboard
        if(now >= next_radio_poll) {

            // Time to poll radio
            //fprintf(stdout, "send_poll...");
            for (int i = 0; i < num_ssrc; i++) {
                //fprintf(stdout, "%d ", ssrclist[i].ssrc);
                send_poll(Ctl_fd,ssrclist[i].ssrc);
            }
            //fprintf(stdout, "\n");

            // Retransmit after 1/10 sec if no response
            next_radio_poll = now + radio_poll_interval + arc4random_uniform(random_interval) - random_interval/2;
        }

        // Poll the input socket
        // This paces keyboard polling so wait no more than 100 ms, even for long refresh intervals
        int const recv_timeout = BILLION/10;
        int64_t start_of_recv_poll = now;
        uint8_t buffer[PKTSIZE];
        int length = 0;
        int ssrc;



        // loop through each of the RTP packets we receive back from our 'poll'.  When we're done looping through these, we can save the data (or print it out).
        do {
            int const npoll = 1;
            struct pollfd pollfd[npoll];
            pollfd[0].fd = Status_fd;
            pollfd[0].events = POLLIN;

            //fprintf(stdout, "poll command...\n");
            poll(pollfd,npoll,100);
            now = gps_time_ns(); // poll() blocks, so update the time of day (only place we block)

            if(pollfd[0].revents & POLLIN) {

                // Message from the radio program (or some transcoders)
                struct sockaddr_storage source_address;
                socklen_t ssize = sizeof(source_address);
                length = recvfrom(Status_fd,buffer,sizeof(buffer),0,(struct sockaddr *)&source_address,&ssize); // should not block
                                                                                                                
                // Ignore our own command packets and responses to other SSIDs
                if(length >= 0 && (enum pkt_type)buffer[0] == STATUS) {
                    ssrc = for_us(buffer+1, length-1);
                    for (int i = 0; i < num_ssrc; i++) {
                        if (ssrclist[i].ssrc == ssrc) {
                            decode_status(&ssrclist[i], buffer+1,length-1);
                            break;
                        }
                    }

                    next_radio_poll = now + radio_poll_interval + arc4random_uniform(random_interval) - random_interval/2;
                }
            }
        } while(now < start_of_recv_poll + recv_timeout && !stop_processing);

        // loop through our list of ssrc's inserting data into the database
        for (int i = 0; i < num_ssrc; i++) {

            // If this is new data, then we publish it (i.e. print it out if we're 'verbose' or save to the database). 
            if (ssrclist[i].Frontend.timestamp > ssrclist[i].lastpoll)
                publishdata(&ssrclist[i]);

            ssrclist[i].lastpoll = ssrclist[i].Frontend.timestamp;
        }
    }

    // Ask the GPS thread to end
    pthread_cancel(gps_thread);
    pthread_join(gps_thread,NULL);

    fprintf(Logfile, "%s Done.\n", format_gpstime(timebuffer, sizeof(timebuffer), gps_time_ns()));
    exit(EXIT_SUCCESS);
}

// parse a string creating an array of integers (ultimate the ssrc's we want to monitor)
ssrcitem_t *parse_int_string(const char *str, int *num_elements)
{
    int capacity = 8;  // Initial capacity for the array
    ssrcitem_t *arr = malloc(capacity * sizeof(ssrcitem_t));
    *num_elements = 0;

    char *token;
    char *dup = strdup(str);  // Avoid modifying the original string

    if (dup == NULL) {
        return NULL; // Memory allocation failed
    }

    // get the first string from the space delimited list
    token = strtok(dup, " ");

    // loop until we run out of tokens (i.e. space delimited string)
    while (token != NULL) {
        if (*num_elements == capacity) {

            // Reallocate array if it reaches capacity
            capacity *= 2;
            arr = realloc(arr, capacity * sizeof(ssrcitem_t));
            if (arr == NULL) {
                free(dup);
                return NULL; // Memory allocation failed
            }
        }

        // parse the token (ex. 144m390, 446m310, etc.).  aka just remove the 'm'.  
        int len = strlen(token);
        char ssrc[len];

        // remove the 'm' from the token and save the result into the ssrc string
        memset(ssrc, 0, sizeof(ssrc));
        rmch(token, 'm', ssrc, sizeof(ssrc));

        // update our array of structs
        arr[*num_elements].Frontend.frequency = NAN;
        arr[*num_elements].Frontend.min_IF = NAN;
        arr[*num_elements].Frontend.max_IF = NAN;
        init_demod(&(arr[*num_elements].Channel));
        arr[*num_elements].ssrc = atoi(ssrc);
        arr[*num_elements].lastpoll = 0;
        (*num_elements)++;

        // get the next token from the list
        token = strtok(NULL, " ");
    }

    free(dup);
    return arr;
}

//  remove a specific character from a string, placing the result in the 'dest' string.
void rmch(char *source, char c, char *dest, size_t dest_size)
{
    long unsigned int i;

    // loop through each char in the source
    for (i = 0; *source != 0 && i < dest_size; source++, i++) {
        if (*source == c)
            // we found the character, 'c'.  We skip anything below and just start again at the top of the loop....
            continue;

        // otherwise, this is a normal character so we write it to the position pointed to by source
        *dest = *source;

        // increment our destination pointer
        dest++;
    }
}

// Initialize a new, unused channel instance where fields start non-zero
static int init_demod(struct channel *channel){
  if(channel == NULL)
    return -1;
  memset(channel,0,sizeof(*channel));
  channel->tune.second_LO = NAN;
  channel->tune.freq = channel->tune.shift = NAN;
  channel->filter.min_IF = channel->filter.max_IF = channel->filter.kaiser_beta = NAN;
  channel->output.headroom = channel->linear.hangtime = channel->linear.recovery_rate = NAN;
  channel->sig.bb_power = channel->sig.snr = channel->sig.foffset = NAN;
  channel->fm.pdeviation = channel->pll.cphase = NAN;
  channel->output.gain = NAN;
  channel->tp1 = channel->tp2 = NAN;
  return 0;
}


// Is response for us (1), or for somebody else (-1)?
//static int for_us(struct channel *channel,uint8_t const *buffer,int length,uint32_t ssrc)
static int for_us(uint8_t const *buffer,int length)
{
    uint8_t const *cp = buffer;

    while(cp - buffer < length) {
        enum status_type const type = *cp++; // increment cp to length field

        if(type == EOL)
            break; // end of list, no length

        unsigned int optlen = *cp++;
        if(optlen & 0x80) {
            // length is >= 128 bytes; fetch actual length from next N bytes, where N is low 7 bits of optlen
            int length_of_length = optlen & 0x7f;
            optlen = 0;
            while(length_of_length > 0) {
                optlen <<= 8;
                optlen |= *cp++;
                length_of_length--;
            }
        }
        if(cp - buffer + optlen >= length)
            break; // invalid length; we can't continue to scan

        switch(type) {
            case EOL: // Shouldn't get here
                return -1;
                break;
            case OUTPUT_SSRC: // If we've specified a SSRC, it must match it
                int decoded_ssrc = decode_int32(cp, optlen);

                // loop through the list of ssrc items to see if we can find a match
                for (int i = 0; i < num_ssrc; i++) {
                    if (ssrclist[i].ssrc == decoded_ssrc)
                        return decoded_ssrc;
                }
                break;
            default:
                break; // Ignore on this pass
        }
        cp += optlen;
    }

    return -1; // not specified, so not for us
}

// Decode incoming status message from the radio program, convert and fill in fields in local channel structure
// Leave all other fields unchanged, as they may have local uses (e.g., file descriptors)
// Note that we use some fields in channel differently than in the radio (e.g., dB vs ratios)
static int decode_status(ssrcitem_t *ssrc, uint8_t const *buffer,int length)
{
    struct frontend *Frontend = &(ssrc->Frontend);
    struct channel *channel = &(ssrc->Channel);

    return decode_radio_status(Frontend, channel, buffer, length);
}

// We use this to print a csv line of data to stdout if we're in verbose mode.  Regardless data is saved to the database.
void publishdata(ssrcitem_t *ssrc)
{
    struct channel *channel = &(ssrc->Channel);
    struct frontend *Frontend = &(ssrc->Frontend);

    float const noise_bandwidth = fabsf(channel->filter.max_IF - channel->filter.min_IF);
    float sig_power = channel->sig.bb_power - noise_bandwidth * channel->sig.n0;

    if(sig_power < 0)
        sig_power = 0; // can happen due to smoothing

    float sn0 = sig_power/channel->sig.n0;
    float snr =  power2dB(sn0/noise_bandwidth);
    if (snr < -100)
        snr = -1;

    // fields
    // timestamp, ssrc, lna_gain, mixer_gain, if_gain, input_power_dbfs, frontend_gain_db, baseband_power_db, snr_dB
    if (!stop_processing) {
        //fprintf(stdout, "sig_power: %f, sig.n0: %f, noise_bandwidth: %f, sn0: %f, snr: %f\n", sig_power, channel->sig.n0, noise_bandwidth, sn0, snr);
        
        double local_gps_lat, local_gps_lon;
        int local_gps_mode = 0;

        // get the GPS mode, lat, and lon
        pthread_mutex_lock(&gps_lock);
        local_gps_mode = gpsmode;
        local_gps_lat = gpslat;
        local_gps_lon = gpslon;
        pthread_mutex_unlock(&gps_lock);

        if (snr > SNR_threshold) {
            db_write(
                Frontend->timestamp,
                local_gps_mode,
                local_gps_mode < 3 ? 0 : local_gps_lat,
                local_gps_mode < 3 ? 0 : local_gps_lon,
                channel->output.rtp.ssrc * 1000,
                Frontend->lna_gain,
                Frontend->mixer_gain,
                Frontend->if_gain,
                power2dB(Frontend->if_power),
                Frontend->rf_atten + Frontend->rf_gain,
                power2dB(channel->sig.bb_power),
                snr
            );
        }

        if (Verbose) {
            char buffer[128];
            struct timespec ts;
            struct tm t;
            ts.tv_sec = ts.tv_nsec = 0;
            ns2ts(&ts, Frontend->timestamp);
            gmtime_r((const time_t *) &(ts.tv_sec), &t);
            memset(buffer, 0, sizeof(buffer));
            fprintf(stdout, "%s, %d, %.6f, %.6f, %d, %02d, %02d, %02d, %.2f, %.2f, %.2f, %.2f\n",
                    format_gpstime(buffer, sizeof(buffer), Frontend->timestamp),
                    local_gps_mode,
                    local_gps_mode < 3 ? 0 : local_gps_lat,
                    local_gps_mode < 3 ? 0 : local_gps_lon,
                    channel->output.rtp.ssrc * 1000,
                    Frontend->lna_gain,
                    Frontend->mixer_gain,
                    Frontend->if_gain,
                    power2dB(Frontend->if_power),
                    Frontend->rf_atten + Frontend->rf_gain,
                    power2dB(channel->sig.bb_power),
                    snr
                   );
        }
    }
}


// closedown the app
void closedown(int x)
{
    if (Verbose)
        fprintf(stdout, "\nStopping app...\n");
    stop_processing = true;
    //sleep(1);
    if (Verbose)
        fprintf(stdout, "Disconnected from database.\n");
    db_term();
}


// the GPS thread
void *gpsthread(void *arg)
{
    pthread_setname("gps-thread");

    // buffer to save timeformatted output
    char timebuffer[1024];

    // we'll store GPS data in this structure.
    struct gps_data_t gps_data;

    // number of attempts to connect to GPSD
    int trycount = 0;

    // GPSD Host possibilities
    char *gpshost = "localhost";

    // outer loop for (re)connecting to the GPSD instance
    while (!stop_processing) {

        if (GPSDHost != NULL)
            gpshost = GPSDHost;

        if (0 != gps_open(gpshost, "2947", &gps_data)) {
            fprintf(Logfile, "%s Unable to connect to GPSD on %s.  trycount=%d\n", format_gpstime(timebuffer, sizeof(timebuffer), gps_time_ns()), gpshost, trycount);

            // sleep for a few microseconds.  Ramping up the sleep time as the trycount gets large (i.e. we've lost connectivity all together...no sense in just continually retrying).
            float usecs = 10 * exp(trycount);
            usleep(usecs);
        }
        else {
            fprintf(Logfile, "%s Connected to GPSD on %s.\n", format_gpstime(timebuffer, sizeof(timebuffer), gps_time_ns()), gpshost);
        }


        (void) gps_stream(&gps_data, WATCH_ENABLE | WATCH_JSON, NULL);
        
        double change_threshold = .0001;
        double prev_lat, prev_lon;
        prev_lat = prev_lon = 0;

        // Loop continuously reading from GPSD
        while (gps_waiting(&gps_data, 5000000) && !stop_processing) {
            if (-1 == gps_read(&gps_data, NULL, 0)) {
                fprintf(Logfile, "%s Unable to read from GPSD on %s.\n", format_gpstime(timebuffer, sizeof(timebuffer), gps_time_ns()), gpshost);
                break;
            }

            // check that a GPS mode has been set.  If not, then just abort and restart at the top of this inner loop.
            if (MODE_SET != (MODE_SET & gps_data.set)) {
                continue;
            }

            // If the lat/lon is valid, then check if we should update the GPS location variables
            if (isfinite(gps_data.fix.latitude) && isfinite(gps_data.fix.longitude)) {

                // only update the our GPS position variables if our position has changed by > 0.0001 degrees, and we have a 3D fix.
                if ((fabs(gps_data.fix.latitude - prev_lat) > change_threshold || fabs(gps_data.fix.longitude - prev_lon) > change_threshold) && gps_data.fix.mode >= 3) {

                    if (Verbose)
                        fprintf(stdout, "GPS position change: %d %.6f, %.6f\n", gps_data.fix.mode, gps_data.fix.latitude, gps_data.fix.longitude);

                    // Update the GPS mode/lat/lon variables
                    pthread_mutex_lock(&gps_lock);
                    gpsmode = gps_data.fix.mode;
                    gpslat = gps_data.fix.latitude;
                    gpslon = gps_data.fix.longitude;
                    pthread_mutex_unlock(&gps_lock);

                }
                
                // reset the trycount since we've just read data from the GPS
                if (trycount > 0) {
                    fprintf(Logfile, "%s Reconnected to GPSD on %s.  GPS mode: %d.\n", format_gpstime(timebuffer, sizeof(timebuffer), gps_time_ns()), gpshost, gps_data.fix.mode);
                    trycount = 0;
                }

                prev_lat = gps_data.fix.latitude;
                prev_lon = gps_data.fix.longitude;

            } 
        }
    }

    // When you are done...
    (void)gps_stream(&gps_data, WATCH_DISABLE, NULL);
    (void)gps_close(&gps_data);
    fprintf(Logfile, "%s GPSD thread ended.\n", format_gpstime(timebuffer, sizeof(timebuffer), gps_time_ns()));

    return NULL;
}


// Send empty poll command on specified descriptor
static int send_poll (int fd, int ssrc) {
  uint8_t cmdbuffer[128];
  uint8_t *bp = cmdbuffer;
  *bp++ = 1; // Command

  uint32_t tag = random();
  encode_int(&bp,COMMAND_TAG,tag);
  encode_int(&bp,OUTPUT_SSRC,ssrc); // poll specific SSRC, or request ssrc list with ssrc = 0
  encode_eol(&bp);
  int const command_len = bp - cmdbuffer;
  if(send(fd, cmdbuffer, command_len, 0) != command_len)
    return -1;

  return 0;
}
