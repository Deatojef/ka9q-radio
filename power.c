// Interactive program to send commands and display internal state of 'radiod'
// Why are user interfaces always the biggest, ugliest and buggiest part of any program?
// Written as one big polling loop because ncurses is **not** thread safe

// Copyright 2017-2023 Phil Karn, KA9Q
// Major revisions fall 2020, 2023 (really continuous revisions!)

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
//static float Refresh_rate = 0.25f;
static float Refresh_rate = .25f;
static struct sockaddr_storage Metadata_dest_address;      // Dest of metadata (typically multicast)

int Mcast_ttl = DEFAULT_MCAST_TTL;
int IP_tos = DEFAULT_IP_TOS;
int Ctl_fd,Status_fd;
const char *App_path;
int Verbose;
float SNR_threshold = 0.0;
bool stop_processing = false;


// This is the list of ssrc's that we're interested and how many of them there are
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
static int decode_radio_status(ssrcitem_t *ssrcitem,uint8_t const *buffer,int length);


// Main routine that loops until killed.
int main(int argc,char *argv[])
{
    App_path = argv[0];
    {
        int c;
        while((c = getopt(argc,argv,"vVs:r:n:y:")) != -1) {
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
                default:
                    fprintf(stdout,"Unknown option %c\n",c);
                    break;
            }
        }
    }

    // capture any sort of kill/close conditions.  Mostly so we can cleanly exit, disconnect from the database, etc..
    signal(SIGPIPE, closedown);
    signal(SIGINT, closedown);
    signal(SIGKILL, closedown);
    signal(SIGQUIT, closedown);
    signal(SIGTERM, closedown);


    // Check that proper arguments were used with starting up
    if(argc <= optind) {
        fprintf(stderr,"Usage: %s -s <ssrc list> mcast_group [-r <refresh rate in secs>] [-v]\n",App_path);
        fprintf(stderr,"<ssrclist> is a list of comma seperated positive decimal numbers, mcast_group is DNS name or IP address of control multicast group\n");
        exit(EX_USAGE);
    }
    if (ssrclist == NULL || num_ssrc == 0) {
        fprintf(stderr,"Missing <ssrc>\n");
        fprintf(stderr,"Usage: %s -s <ssrc> mcast_group [-r <refresh rate in secs>] [-v]\n",App_path);
        fprintf(stderr,"<ssrclist> is a list of comma seperated positive decimal numbers, mcast_group is DNS name or IP address of control multicast group\n");
        exit(EX_USAGE);
    }


    // try and log into the database.  If unable to connect to the DB, then we can't proceed.
    if (db_init()) {
        if (Verbose)
            fprintf(stdout, "database connection successful\n");
    } 
    else {
        fprintf(stderr, "Unable to connect to database\n");
        exit(EX_IOERR);
    }

    // This is the status RTP address (ex. 2m.local, 70cm.local)
    char iface[1024]; // Multicast interface
    resolve_mcast(argv[optind],&Metadata_dest_address,DEFAULT_STAT_PORT,iface,sizeof(iface));
    Status_fd = listen_mcast(&Metadata_dest_address,iface);
    
    // If unable to connect to the status multicast address then exit
    if(Status_fd == -1) {
        fprintf(stderr,"Can't listen to mcast status %s\n",argv[optind]);
        exit(EX_IOERR);
    }

    // this is the same status RTP address, except that we need to use this file descriptor to send a "poll" command to multicast group.
    Ctl_fd = connect_mcast(&Metadata_dest_address,iface,Mcast_ttl,IP_tos);
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
                                                                                                                //
                // Ignore our own command packets and responses to other SSIDs
                if(length >= 0 && (enum pkt_type)buffer[0] == STATUS) {
                    ssrc = for_us(buffer+1, length-1);
                    for (int i = 0; i < num_ssrc; i++) {
                        if (ssrclist[i].ssrc == ssrc) {
                            decode_radio_status(&ssrclist[i], buffer+1,length-1);
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

    fprintf(stdout, "Done.\n");
    exit(EXIT_SUCCESS);
}

// parse a string creating an array of integers
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

    token = strtok(dup, ",");
    while (token != NULL) {
        if (*num_elements == capacity) {

            // Reallocate array if it reaches capacity
            capacity *= 2;
            arr = realloc(arr, capacity * sizeof(int));
            if (arr == NULL) {
                free(dup);
                return NULL; // Memory allocation failed
            }
        }
        arr[*num_elements].Frontend.frequency = NAN;
        arr[*num_elements].Frontend.min_IF = NAN;
        arr[*num_elements].Frontend.max_IF = NAN;
        init_demod(&(arr[*num_elements].Channel));
        arr[*num_elements].ssrc = atoi(token);
        arr[*num_elements].lastpoll = 0;
        (*num_elements)++;
        token = strtok(NULL, ",");
    }

    free(dup);
    return arr;
}

// Initialize a new, unused channel instance where fields start non-zero
static int init_demod(struct channel *channel)
{
    memset(channel,0,sizeof(*channel));
    channel->tune.second_LO = NAN;
    channel->tune.freq = channel->tune.shift = NAN;
    channel->filter.min_IF = channel->filter.max_IF = channel->filter.kaiser_beta = NAN;
    channel->output.headroom = channel->linear.hangtime = channel->linear.recovery_rate = NAN;
    channel->sig.bb_power = channel->sig.snr = channel->sig.foffset = NAN;
    channel->fm.pdeviation = channel->linear.cphase = channel->linear.lock_timer = NAN;
    channel->output.gain = NAN;
    channel->tp1 = channel->tp2 = NAN;

    channel->output.data_fd = channel->output.rtcp_fd = -1;
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
static int decode_radio_status(ssrcitem_t *ssrc, uint8_t const *buffer,int length)
{
    struct frontend *Frontend = &(ssrc->Frontend);
    struct channel *channel = &(ssrc->Channel);

    uint8_t const *cp = buffer;
    while(cp - buffer < length) {
        enum status_type type = *cp++; // increment cp to length field

        if(type == EOL)
            break; // end of list

        unsigned int optlen = *cp++;
        if(cp - buffer + optlen >= length)
            break; // invalid length; we can't continue to scan

        //fprintf(stdout, "status type:  %d\n", type);
        switch(type) {
            case EOL:
                break;
            case CMD_CNT:
                channel->commands = decode_int32(cp,optlen);
                break;
            case DESCRIPTION:
                FREE(Frontend->description);
                Frontend->description = decode_string(cp,optlen);
                break;
            case GPS_TIME:
                Frontend->timestamp = decode_int64(cp,optlen);
                break;
            case INPUT_SAMPRATE:
                Frontend->samprate = decode_int(cp,optlen);
                break;
            case INPUT_SAMPLES:
                Frontend->samples = decode_int64(cp,optlen);
                break;
            case AD_OVER:
                Frontend->overranges = decode_int64(cp,optlen);
                break;
            case OUTPUT_DATA_SOURCE_SOCKET:
                decode_socket(&channel->output.data_source_address,cp,optlen);
                break;
            case OUTPUT_DATA_DEST_SOCKET:
                decode_socket(&channel->output.data_dest_address,cp,optlen);
                break;
            case OUTPUT_SSRC:
                int s = decode_int32(cp, optlen);
                channel->output.rtp.ssrc = s;
                break;
            case OUTPUT_TTL:
                Mcast_ttl = decode_int8(cp,optlen);
                break;
            case OUTPUT_SAMPRATE:
                channel->output.samprate = decode_int(cp,optlen);
                break;
            case OUTPUT_DATA_PACKETS:
                channel->output.rtp.packets = decode_int64(cp,optlen);
                break;
            case OUTPUT_METADATA_PACKETS:
                ssrc->Metadata_packets = decode_int64(cp,optlen);
                break;
            case FILTER_BLOCKSIZE:
                Frontend->L = decode_int(cp,optlen);
                break;
            case FILTER_FIR_LENGTH:
                Frontend->M = decode_int(cp,optlen);
                break;
            case LOW_EDGE:
                channel->filter.min_IF = decode_float(cp,optlen);
                break;
            case HIGH_EDGE:
                channel->filter.max_IF = decode_float(cp,optlen);
                break;
            case FE_LOW_EDGE:
                Frontend->min_IF = decode_float(cp,optlen);
                break;
            case FE_HIGH_EDGE:
                Frontend->max_IF = decode_float(cp,optlen);
                break;
            case FE_ISREAL:
                Frontend->isreal = decode_int8(cp,optlen) ? true: false;
                break;
            case AD_BITS_PER_SAMPLE:
                Frontend->bitspersample = decode_int(cp,optlen);
                break;
            case IF_GAIN:
                Frontend->if_gain = decode_int8(cp,optlen);
                break;
            case LNA_GAIN:
                Frontend->lna_gain = decode_int8(cp,optlen);
                break;
            case MIXER_GAIN:
                Frontend->mixer_gain = decode_int8(cp,optlen);
                break;
            case KAISER_BETA:
                channel->filter.kaiser_beta = decode_float(cp,optlen);
                break;
            case FILTER_DROPS:
                ssrc->Block_drops = decode_int(cp,optlen);
                break;
            case IF_POWER:
                Frontend->if_power = dB2power(decode_float(cp,optlen));
                break;
            case BASEBAND_POWER:
                channel->sig.bb_power = dB2power(decode_float(cp,optlen)); // dB -> power
                break;
            case NOISE_DENSITY:
                channel->sig.n0 = dB2power(decode_float(cp,optlen));
                break;
            case DEMOD_SNR:
                channel->sig.snr = dB2power(decode_float(cp,optlen));
                break;
            case FREQ_OFFSET:
                channel->sig.foffset = decode_float(cp,optlen);
                break;
            case PEAK_DEVIATION:
                channel->fm.pdeviation = decode_float(cp,optlen);
                break;
            case PLL_LOCK:
                channel->linear.pll_lock = decode_int8(cp,optlen);
                break;
            case PLL_BW:
                channel->linear.loop_bw = decode_float(cp,optlen);
                break;
            case PLL_SQUARE:
                channel->linear.square = decode_int8(cp,optlen);
                break;
            case PLL_PHASE:
                channel->linear.cphase = decode_float(cp,optlen);
                break;
            case ENVELOPE:
                channel->linear.env = decode_int8(cp,optlen);
                break;
            case OUTPUT_LEVEL:
                channel->output.energy = dB2power(decode_float(cp,optlen));
                break;
            case OUTPUT_SAMPLES:
                channel->output.samples = decode_int64(cp,optlen);
                break;
            case COMMAND_TAG:
                channel->command_tag = decode_int32(cp,optlen);
                break;
            case RADIO_FREQUENCY:
                channel->tune.freq = decode_double(cp,optlen);
                break;
            case SECOND_LO_FREQUENCY:
                channel->tune.second_LO = decode_double(cp,optlen);
                break;
            case SHIFT_FREQUENCY:
                channel->tune.shift = decode_double(cp,optlen);
                break;
            case FIRST_LO_FREQUENCY:
                Frontend->frequency = decode_double(cp,optlen);
                break;
            case DOPPLER_FREQUENCY:
                channel->tune.doppler = decode_double(cp,optlen);
                break;
            case DOPPLER_FREQUENCY_RATE:
                channel->tune.doppler_rate = decode_double(cp,optlen);
                break;
            case DEMOD_TYPE:
                channel->demod_type = decode_int(cp,optlen);
                break;
            case OUTPUT_CHANNELS:
                channel->output.channels = decode_int(cp,optlen);
                break;
            case INDEPENDENT_SIDEBAND:
                channel->filter.isb = decode_int8(cp,optlen);
                break;
            case THRESH_EXTEND:
                channel->fm.threshold = decode_int8(cp,optlen);
                break;
            case PLL_ENABLE:
                channel->linear.pll = decode_int8(cp,optlen);
                break;
            case GAIN:              // dB to voltage
                channel->output.gain = dB2voltage(decode_float(cp,optlen));
                break;
            case AGC_ENABLE:
                channel->linear.agc = decode_int8(cp,optlen);
                break;
            case HEADROOM:          // db to voltage
                channel->output.headroom = dB2voltage(decode_float(cp,optlen));
                break;
            case AGC_HANGTIME:      // s to samples
                channel->linear.hangtime = decode_float(cp,optlen);
                break;
            case AGC_RECOVERY_RATE: // dB/s to dB/sample to voltage/sample
                channel->linear.recovery_rate = dB2voltage(decode_float(cp,optlen));
                break;
            case AGC_THRESHOLD:   // dB to voltage
                channel->linear.threshold = dB2voltage(decode_float(cp,optlen));
                break;
            case TP1: // Test point
                channel->tp1 = decode_float(cp,optlen);
                break;
            case TP2:
                channel->tp2 = decode_float(cp,optlen);
                break;
            case SQUELCH_OPEN:
                channel->squelch_open = dB2power(decode_float(cp,optlen));
                break;
            case SQUELCH_CLOSE:
                channel->squelch_close = dB2power(decode_float(cp,optlen));
                break;
            case DEEMPH_GAIN:
                channel->deemph.gain = decode_float(cp,optlen);
                break;
            case DEEMPH_TC:
                channel->deemph.rate = 1e6*decode_float(cp,optlen);
                break;
            case PL_TONE:
                channel->fm.tone_freq = decode_float(cp,optlen);
                break;
            case PL_DEVIATION:
                channel->fm.tone_deviation = decode_float(cp,optlen);
                break;
            case NONCOHERENT_BIN_BW:
                channel->spectrum.bin_bw = decode_float(cp,optlen);
                break;
            case BIN_COUNT:
                channel->spectrum.bin_count = decode_int(cp,optlen);
                break;
            case BIN_DATA:
                break;
            case RF_GAIN:
                Frontend->rf_gain = decode_float(cp,optlen);
                break;
            case RF_ATTEN:
                Frontend->rf_atten = decode_float(cp,optlen);
                break;
            case BLOCKS_SINCE_POLL:
                channel->blocks_since_poll = decode_int64(cp,optlen);
                break;
            case PRESET: {
                char *p = decode_string(cp,optlen);
                strlcpy(channel->preset,p,sizeof(channel->preset));
                FREE(p);
            }
            break;
            case RTP_PT:
                channel->output.rtp.type = decode_int(cp,optlen);
                break;
            default: // ignore others
                break;
        }
        cp += optlen;
    }
    return 0;
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
        local_gps_mode = gpsmode;
        local_gps_lat = gpslat;
        local_gps_lon = gpslon;

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
            fprintf(stdout, "%s, %d, %.6f, %.6f, %d, %02d, %02d, %02d, %.1f, %.1f, %.1f, %.1f\n",
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
    sleep(1);
    if (Verbose)
        fprintf(stdout, "Disconnected from database.\n");
    db_term();
}


// the GPS thread
void *gpsthread(void *arg)
{
    pthread_setname("gps-thread");

    // we'll store GPS data in this structure.
    struct gps_data_t gps_data;

    // number of attempts to connect to GPSD
    int trycount = 0;

    // outer loop for (re)connecting to the GPSD instance
    while (!stop_processing) {

        if (0 != gps_open("localhost", "2947", &gps_data)) {
            fprintf(stderr, "Unable to connect to GPSD.  trycount=%d.\n", trycount);

            // sleep for a few microseconds.  Ramping up the sleep time as the trycount gets large (i.e. we've lost connectivity all together...no sense in just continually retrying).
            float usecs = 10 * exp(trycount);
            usleep(usecs);
        }

        (void) gps_stream(&gps_data, WATCH_ENABLE | WATCH_JSON, NULL);
        
        double change_threshold = .0001;
        double prev_lat, prev_lon;
        prev_lat = prev_lon = 0;

        // Loop continuously reading from GPSD
        while (gps_waiting(&gps_data, 5000000) && !stop_processing) {
            if (-1 == gps_read(&gps_data, NULL, 0)) {
                fprintf(stderr, "Unable to read from GPSD.\n");
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
                    fprintf(stderr, "Reconnected to GPSD. GPS Mode: %d\n", gps_data.fix.mode);
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
    fprintf(stdout, "GPS thread ended.\n");

    return NULL;
}
