// igate
//
// Functionally just an Rx-only igate. 
// Process AX.25 frames containing APRS data, feed to APRS2 network
//
// Copyright 2017-2024 Phil Karn, KA9Q & Jeff Deaton, N6BA
// Major revisions fall 2020, 2023, 2024 (really continuous revisions!)

#define _GNU_SOURCE 1
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>

#include <locale.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>
#include <math.h>
#include <ctype.h>
#if __linux__
#include <bsd/string.h>
#else
#include <string.h>
#endif
#include <signal.h>
#include <sysexits.h>
#include <poll.h>
#include <gps.h>
#include <cjson/cJSON.h>
#include <libmemcached/memcached.h>

#include "multicast.h"
#include "ax25.h"
#include "misc.h"


// structures for APRS telemetry data and station particulars
// ---------------------------------------------------------------------
typedef struct {
    double a;
    double b;
    double c;
} APRS_COEF;

typedef struct {
    APRS_COEF rx;
    APRS_COEF drp;
    APRS_COEF rxsat;
    APRS_COEF rxdirect;
} APRS_EQNS;

typedef struct {
    double lat;
    double lon;
    double alt;
    double course;
    double speed;
    int gpsmode; // By default, not using GPSD for location information
} LOCATION;

// APRS packet stats
typedef struct {
    int dropped;  // number of packets dropped (i.e. not sent to APRS-IS) for one reason or another
    int received;  // number of received (over RF...err..technical from packetd) packets
    int satellite;  // number of packets received from satellites (mostly on 145.825MHz)
    int direct;  // number of packets we've heard directly (i.e. not digipeated)
} PACKET_STATISTICS;

// this igate configuration
typedef struct {

    // is this a fixed or mobile station
    bool ismobile;  // default to a fixed station
                            
    // GPSD hostname
    char gpsdhost[128];
    bool gpsdenabled;

    // save packets heard to memcached?
    bool usememcache;  // default to NOT using memcached

    // AX25 multicast hostname
    char ax25host[128];

    // APRS-IS hostname and port
    char aprsishost[128];
    char aprsisport[16];
    bool aprsisbeaconing;

    // The name of the telementry sequence filename
    char telemseqfilename[128];
    int sequence;

    // the name of the log file
    char logfilename[128]; 

    // Ham callsign and passcode
    char callsign[16];
    char passcode[16];

    char symbol[3];  // character representing the symbol
    char overlay[3]; // character representing the char to be overlaid on top of the symbol
    char comment[186]; // max APRS information field length is 256 bytes. That minus normal position packet length (ex. 70 bytes), leaves 186 bytes for "everything else" like the station comment.

    // igate configuration file
    char configfilename[128];

    // file handles
    FILE *logfile;  // log file
    FILE *configfile;  // config file

} IGATE_CONFIGURATION;

// ---------------------------------------------------------------------


// global variables
// ---------------------------------------------------------------------
char *Mcast_address_text = "ax25.local";
const char *App_path;
char *Logfilename;
char *Configfilename = "/etc/radio/igate.json";

// max packet size for position packets (actually the max size is 256 - 70 = 186, but backing this off by 8 bytes for a little buffer room)
static int MAX_POSIT = 178;

// APRS tocall value.  Experimental tocalls start with APZxxx.  Using "KR1" for "KA9Q-Radio 1".  Dunno...just making this up.  ;)
char *tocall = "APZKR1";


// the configuration
IGATE_CONFIGURATION igate_configuration;

// statistics (this is protected by a mutex)
PACKET_STATISTICS aprs_statistics;

// location data (this is protected by a mutex)
LOCATION location;

// Verbosity level
int Verbose;

// File descriptors for connecting to RTP streams.
int Input_fd = -1;
int Network_fd = -1;

// The global variable that processing loops check.  If this is set to "true", then processing loops should end.
bool stop_processing = false;
// ---------------------------------------------------------------------


// Thread mutex, conditions, and threads
// ---------------------------------------------------------------------
pthread_mutex_t tcp_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t beacon_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t location_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cleartoxmit = PTHREAD_COND_INITIALIZER;
pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;

pthread_t Beacon_thread;
pthread_t Read_thread;
pthread_t GPSD_thread;
// ---------------------------------------------------------------------


// Functions
// ---------------------------------------------------------------------
void *netreader(void *arg);
void *beaconthread(void *arg);
void *gpsthread(void *arg);
int positpacket(char *buffer, size_t n, char *info);
int bitspacket(char *buffer, size_t n);
int unitspacket(char *buffer, size_t n);
int parampacket(char *buffer, size_t n);
int eqnpacket(char *buffer, size_t n, APRS_EQNS *eqns);
int calculatecoef(double value, APRS_COEF *c);
int telemetrypacket(char *buffer, size_t n, APRS_EQNS *eqns);
int createinfostring(char *buffer, size_t n, LOCATION *station);
void closedown(int x);
int readtelemseq(char *filename);
void writetelemseq(char *filename, int seq);
void processJSONObject(cJSON *json);
int passcode(char *call, char *buffer, size_t n_buffer);
void checkForConfigurable(cJSON *item);
void initlocation(LOCATION *loc);
void initpacketstatistics(PACKET_STATISTICS *stats);
void initigateconfig(IGATE_CONFIGURATION *config);
// --------------------------------------------------------------------


// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// main function
// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
int main(int argc,char *argv[])
{
    char timebuffer[1024];

    App_path = argv[0];

    // Quickly drop root if we have it
    // The sooner we do this, the fewer options there are for abuse
    if(seteuid(getuid()) != 0)
        fprintf(stderr,"seteuid: %s\n",strerror(errno));

    memset(timebuffer, 0, sizeof(timebuffer));
    setlocale(LC_ALL,getenv("LANG"));
    setlinebuf(stdout);

    // capture any sort of kill/close conditions.  Mostly so we can cleanly exit
    signal(SIGPIPE, closedown);
    signal(SIGINT, closedown);
    signal(SIGKILL, closedown);
    signal(SIGQUIT, closedown);
    signal(SIGTERM, closedown);

    // Parse through command line arguments
    int c;
    while((c = getopt(argc,argv,"vVf:c:")) != EOF) {
        switch(c) {
            case 'f':
                Logfilename = optarg;
                Verbose = 0;
                break;
            case 'c':
                Configfilename = optarg;
                break;
            case 'v':
                if(!Logfilename)
                    Verbose++;
                break;
            case 'V':
                VERSION();
                exit(EX_OK);
            default:
                fprintf(stderr,"Usage: %s [-v] [-V] [-f <log file>] [-c <config file>]\n",argv[0]);
                exit(EX_USAGE);
        }
    }


    // --------------------- start: read configuration file ------------------
    //

    // initialize global structures
    initigateconfig(&igate_configuration);
    initlocation(&location);
    initpacketstatistics(&aprs_statistics);
    

    // try and open the configuration file (if it was given on the command line)
    if (Configfilename) {
        snprintf(igate_configuration.configfilename, sizeof(igate_configuration.configfilename), "%s", Configfilename);
        igate_configuration.configfile = fopen(igate_configuration.configfilename, "r");
    }

    // parse through the JSON within the configuration file
    if (igate_configuration.configfile) {
        char buffer[4096];
        int len;

        len = fread(buffer, 1, sizeof(buffer), igate_configuration.configfile);
        fclose(igate_configuration.configfile);

        // parse the JSON data
        cJSON *json = cJSON_Parse(buffer);
        if (json == NULL || len == 0) {
            const char *error_ptr = cJSON_GetErrorPtr();
            if (error_ptr != NULL) {
                fprintf(stderr, "Error reading configuration file: %s\n", error_ptr);
            }
            cJSON_Delete(json);
            exit(EX_IOERR);
        }

        // This will process any json from the configuration file, and populate the igate_configuration structure
        processJSONObject(json);

    }
    else {
        fprintf (stderr, "Unable to open configuration file. Filename not specified.\n");
        exit(EX_SOFTWARE);
    }
    
    // --------------------- end: read configuration file ------------------

    // If a log filename was supplied on the command line, then we copy that into the igate_configuration structure
    if(Logfilename) {
        snprintf (igate_configuration.logfilename, sizeof(igate_configuration.logfilename), "%s", Logfilename);
        igate_configuration.logfile = fopen(igate_configuration.logfilename,"a");
    }
    else // otherwise, we just write to stdout
        igate_configuration.logfile = stdout;

    // Try and write to the log file.  This will shake out any permissions issues writing to that file before we get too far along.
    if (igate_configuration.logfile) {
        setlinebuf(igate_configuration.logfile);
        format_gpstime(timebuffer, sizeof(timebuffer), gps_time_ns());
        fprintf(igate_configuration.logfile,"%s ################## START:  igate ##############\n", timebuffer);
    }
    else {
        fprintf(stderr, "Unable to write to log file:  %s\n", igate_configuration.logfilename);
        exit(EX_IOERR);
    }

    // check the callsign
    if(strlen(igate_configuration.callsign) == 0) {
        fprintf(stderr,"Must specify a callsign for to use as the user when igating to APRS-IS.\n");
        exit(EX_USAGE);
    }


    // if no APRS-IS passcode was supplied, then determine it on the fly
    if(strlen(igate_configuration.passcode) == 0) {
        if (passcode(igate_configuration.callsign, igate_configuration.passcode, sizeof(igate_configuration.passcode) <= 0)) {
            fprintf(stderr,"Unexpected error in computing passcode\n");
            exit(EX_SOFTWARE);
        }
    }


    // Log if this is a mobile or static station
    format_gpstime(timebuffer, sizeof(timebuffer), gps_time_ns());
    fprintf(igate_configuration.logfile, "%s %s station behavior selected.\n", timebuffer, (igate_configuration.ismobile ? "mobile" : "static"));

    // if beaconing was enabled, then try and read the telemetry sequency file
    if (igate_configuration.aprsisbeaconing) { 

        // get the starting sequence number for telemetry packets sent to the APRS-IS server
        igate_configuration.sequence = readtelemseq(igate_configuration.telemseqfilename);

        // get timestamp for printing output below
        format_gpstime(timebuffer, sizeof(timebuffer), gps_time_ns());
        fprintf(igate_configuration.logfile, "%s Beaconing to %s enabled.  Initial telemetry sequence: %d\n", 
                timebuffer, 
                igate_configuration.aprsishost, 
                igate_configuration.sequence
                );

        // log our symbol, overlay, & comment strings...but only since we're beaconing, otherwise they don't matter
        fprintf(igate_configuration.logfile, "%s Station identification:  symbol=%s, overlay=%s, comment=%s\n", 
                timebuffer, 
                igate_configuration.symbol, 
                igate_configuration.overlay, 
                igate_configuration.comment
                );

        // print out a message about how we're using GPSD (or not)
        if (igate_configuration.gpsdenabled) 
            fprintf(igate_configuration.logfile, "%s Using GPSD on %s for location of this station.\n", timebuffer, igate_configuration.gpsdhost);
        else
            fprintf(igate_configuration.logfile, "%s Using provided latitude=%f, longitude=%f, altitude=%f\n", 
                    timebuffer, 
                    location.lat, 
                    location.lon, 
                    location.alt
                    );
    }
    else {  // beaconing was not enabled
        fprintf(igate_configuration.logfile, "%s Not configured to beacon to APRS-IS.\n", format_gpstime(timebuffer, sizeof(timebuffer), gps_time_ns()));
    }


    // Basically loop until we're supposed stop
    while(!stop_processing) {

        // -----------------------------------
        // start:  resolve and connect to aprs-is server
        //
        // Resolve and connect to the APRS network server
        
        struct addrinfo hints;
        memset(&hints,0,sizeof(hints));
        hints.ai_family = PF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        hints.ai_flags = AI_CANONNAME|AI_ADDRCONFIG;

        struct addrinfo *results = NULL;
        int ecode;

        // Try a few times in case we come up before the resolver is quite ready
        for(int tries=0; tries < 10; tries++) {
            if((ecode = getaddrinfo(igate_configuration.aprsishost,igate_configuration.aprsisport,&hints,&results)) == 0)
                break;
            fprintf(igate_configuration.logfile, "%s resolver loop sleeping for 500ms.\n", format_gpstime(timebuffer,sizeof(timebuffer), gps_time_ns()));
            struct timespec tv;
            tv.tv_sec = 0;
            tv.tv_nsec = 5 * 100000000ll;
            //usleep(500000); // 500 ms
            nanosleep(&tv, NULL);
        }

        if(ecode != 0) {
            fprintf(stderr,"Can't getaddrinfo(%s,%s): %s\n",igate_configuration.aprsishost,igate_configuration.aprsisport,gai_strerror(ecode));
            fprintf(igate_configuration.logfile, "%s ecode sleeping for 5s.\n", format_gpstime(timebuffer,sizeof(timebuffer), gps_time_ns()));

            struct timespec tv;
            tv.tv_sec = 5;
            tv.tv_nsec = 0;
            nanosleep(&tv, NULL);
            //usleep(5000000); // 5 sec
            continue; // Keep trying
        }

        struct addrinfo *resp;
        for(resp = results; resp != NULL; resp = resp->ai_next) {
            if((Network_fd = socket(resp->ai_family,resp->ai_socktype,resp->ai_protocol)) < 0)
                continue;
            if(connect(Network_fd,resp->ai_addr,resp->ai_addrlen) == 0)
                break;
            close(Network_fd);
            Network_fd = -1;
        }

        if(resp == NULL) {
            fprintf(stderr,"Can't connect to server %s:%s\n", igate_configuration.aprsishost, igate_configuration.aprsisport);
            fprintf(igate_configuration.logfile, "%s Can't connect to server %s:%s, sleeping for 2mins.\n", 
                    format_gpstime(timebuffer,sizeof(timebuffer), gps_time_ns()), 
                    igate_configuration.aprsishost, 
                    igate_configuration.aprsisport
                    );

            struct timespec tv;
            tv.tv_sec = 60*2;
            tv.tv_nsec = 0;
            nanosleep(&tv, NULL);
            //sleep(600); // 5 minutes
            freeaddrinfo(results);
            resp = results = NULL;
            continue;
        }

        // end:  resolve and connect to aprs-is server
        // --------------------------------------

        format_gpstime(timebuffer, sizeof(timebuffer), gps_time_ns());
        fprintf(igate_configuration.logfile,"%s Connected to APRS server %s port %s\n", timebuffer, resp->ai_canonname,igate_configuration.aprsisport);

        freeaddrinfo(results);
        resp = results = NULL;

        FILE *network = fdopen(Network_fd,"w+");
        setlinebuf(network);

        // start threads
        pthread_create(&Read_thread,NULL,netreader,NULL);
        if (igate_configuration.aprsisbeaconing)
            pthread_create(&Beacon_thread,NULL,beaconthread,NULL);
        if (igate_configuration.gpsdenabled)
            pthread_create(&GPSD_thread,NULL,gpsthread,NULL);


        // Log into the APRS-IS server 
        if(fprintf(network,"user %s pass %s vers KA9Q-aprs 1.0\r\n",igate_configuration.callsign,igate_configuration.passcode) <= 0) {

            // if there was error, then we close our connection, sleep for 5mins, then restart this outer 'while loop' back at the beginning.
            fclose(network);
            network = NULL;

            fprintf(igate_configuration.logfile, "%s login step.  sleeping for 5mins.\n", format_gpstime(timebuffer,sizeof(timebuffer), gps_time_ns()));
            struct timespec tv;
            tv.tv_sec = 600;
            tv.tv_nsec = 0;
            nanosleep(&tv, NULL);
            //sleep(600); // 5 minutes;
            continue;
        }


        // close this file descriptor just in case it was open from a prior loop iteration
        //if (Input_fd != -1 && Input_fd != NULL) {
        //    fclose(Input_fd);
        //}

        // Set up multicast input
        {
            struct sockaddr_storage sock;
            resolve_mcast(Mcast_address_text, &sock, DEFAULT_RTP_PORT, NULL, 0, 0);
            Input_fd = listen_mcast(&sock,NULL);
        }
        if (Input_fd == -1) {
            fprintf(stderr,"Can't set up multicast input from %s\n",Mcast_address_text);
            exit(EX_IOERR);
        }

        uint8_t packet[PKTSIZE];
        int size;

        // loop until we've been signaled to stop processing
        while(!stop_processing) {

            struct pollfd pollfd[1];
            pollfd[0].fd = Input_fd;
            pollfd[0].events = POLLIN;

            // wait for 50ms for new data to appear on the input socket (i.e. the multicast RTP stream)
            poll(pollfd, 1, 50);

            // if there was data available then read it.
            if(pollfd[0].revents & POLLIN) {
                // there *should* be data available on the socket, soooo, this shouldn't block.
                size = recv(Input_fd, packet, sizeof(packet), 0);
            }
            else {
                // no data available on the socket, go back to the start of the loop
                continue;
            }


            struct rtp_header rtp_header;
            uint8_t const *dp = packet;

            dp = ntoh_rtp(&rtp_header,dp);
            size -= dp - packet;

            if(rtp_header.pad) {
                // Remove padding
                size -= dp[size-1];
                rtp_header.pad = 0;
            }

            if(size <= 0)
                continue;  // Bogus RTP header?

            if(rtp_header.type != AX25_pt)
                continue; // Wrong type

            // print the start of a log message.  this includes timestamp, etc..but no newline...as that'll be written down below...
            // Emit local timestamp
            char result[1024];
            fprintf(igate_configuration.logfile,"%s ssrc %u seq %d",
                    format_gpstime(result,sizeof(result),gps_time_ns()),
                    rtp_header.ssrc,rtp_header.seq);

            // Parse incoming AX.25 frame
            struct ax25_frame frame;
            if(ax25_parse(&frame,dp,size) < 0) {
                fprintf(igate_configuration.logfile," Unparsable packet\n");

                pthread_mutex_lock(&stats_lock);
                aprs_statistics.dropped++;
                pthread_mutex_unlock(&stats_lock);

                // restart at the beginning of this inner loop as this was an unparsable AX25 frame
                continue;
            }


            // Construct TNC2-style monitor string for APRS reporting
            char monstring[2048]; // Should be large enough for any legal AX.25 frame; we'll assert this periodically
            int sspace = sizeof(monstring);
            int infolen = 0;
            int is_satellite = 0;
            int used_digis = 0; // number of addresses that have their h-bit set
            int heard_direct = 1; // assume by default that we're the first station to hear this packet
            int is_rfonly = 0;


            // ------------------- start:  process the incoming packet ---------------
            {
                memset(monstring,0,sizeof(monstring));
                char *cp = monstring;
                {
                    int w = snprintf(cp,sspace,"%s>%s",frame.source,frame.dest);
                    cp += w;
                    sspace -= w;
                    assert(sspace > 0);
                }

                // loop through each digipeater address checking for TCPIP and satellite packets heard directly
                for(int i=0; i<frame.ndigi; i++) {

                    // if TCPIP, RFONLY, or NOGATE appears, this frame should not be igated
                    if (strcmp(frame.digipeaters[i].name,"TCPIP") == 0 || strcmp(frame.digipeaters[i].name, "RFONLY") == 0 || strcmp(frame.digipeaters[i].name, "NOGATE") == 0)
                        is_rfonly = 1;
                    else if (strcmp(frame.digipeaters[i].name, "ARISS") == 0 || strcmp(frame.digipeaters[i].name, "RS0ISS") == 0 || rtp_header.ssrc == 145825)
                        is_satellite = 1;
                    
                    int const w = snprintf(cp,sspace,",%s%s",frame.digipeaters[i].name,frame.digipeaters[i].h ? "*" : "");

                    // sum up the number of used digipeaters
                    used_digis += frame.digipeaters[i].h;

                    cp += w;
                    sspace -= w;
                    assert(sspace > 0);
                }

                // if the number of digipeaters with their h-bit set is > 0, then we obviously haven't heard this packet directly (i.e. we're hearing it after it was digipeated).
                if (used_digis)
                    heard_direct = 0;

                // print out if we heard this station directly or not
                fprintf(igate_configuration.logfile, " direct %d", heard_direct);

                {
                    // qAR means a bidirectional i-gate, qAO means receive-only
                    //    w = snprintf(cp,sspace,",qAR,%s",igate_configuration.callsign);
                    int const w = snprintf(cp,sspace,",qAO,%s",igate_configuration.callsign);
                    cp += w;
                    sspace -= w;
                    *cp++ = ':';
                    sspace--;
                    assert(sspace > 0);
                }
                for(int i=0; i < frame.info_len; i++) {
                    char const c = frame.information[i] & 0x7f; // Strip parity in monitor strings
                    if(c != '\r' && c != '\n' && c != '\0') {
                        // Strip newlines, returns and nulls (we'll add a cr-lf later)
                        *cp++ = c;
                        sspace--;
                        infolen++;
                        assert(sspace > 0);
                    }
                }
                *cp++ = '\0';
                sspace--;
            }
            // ------------------- end:  process the incoming packet ---------------


            // make sure there wasn't something odd happening with the buffers
            assert(sizeof(monstring) - sspace - 1 == strlen(monstring));

            // ------------------ start:  stats ------------------------------
            
            // if heard directly, then update that counter
            if (heard_direct) {
                pthread_mutex_lock(&stats_lock);
                aprs_statistics.direct++;
                pthread_mutex_unlock(&stats_lock);
            }

            pthread_mutex_lock(&stats_lock);
            aprs_statistics.received++;
            if (rtp_header.ssrc == 145825)
                aprs_statistics.satellite++;
            pthread_mutex_unlock(&stats_lock);

            // ------------------ end:  stats ------------------------------
            

            // ------------------- start:  check for drop conditions ---------------
            if(frame.control != 0x03 || frame.type != 0xf0) {
                fprintf(igate_configuration.logfile," %s -- Not relaying: invalid ax25 ctl/protocol\n", monstring);

                pthread_mutex_lock(&stats_lock);
                aprs_statistics.dropped++;
                pthread_mutex_unlock(&stats_lock);

                continue;
            }
            if(infolen == 0) {
                fprintf(igate_configuration.logfile," %s -- Not relaying: empty I field\n", monstring);

                pthread_mutex_lock(&stats_lock);
                aprs_statistics.dropped++;
                pthread_mutex_unlock(&stats_lock);

                continue;
            }
            if(is_rfonly) {
                fprintf(igate_configuration.logfile," %s -- Not relaying: RF only or TCPIP packet\n", monstring);

                pthread_mutex_lock(&stats_lock);
                aprs_statistics.dropped++;
                pthread_mutex_unlock(&stats_lock);

                continue;
            }
            if(frame.information[0] == '{') {
                fprintf(igate_configuration.logfile," %s -- Not relaying: third party traffic\n", monstring);

                pthread_mutex_lock(&stats_lock);
                aprs_statistics.dropped++;
                pthread_mutex_unlock(&stats_lock);

                continue;
            }
            if (is_satellite && heard_direct && strcmp(frame.source, "RS0ISS") != 0) {
                fprintf(igate_configuration.logfile," %s -- Not relaying: satellite packet heard directly\n", monstring);
                continue;
            }

            // ------------------- end:  check for drop conditions ---------------

            // print the rest of the log message
            fprintf(igate_configuration.logfile," %s\n", monstring);

            int ret;

            // Send to APRS network with appended crlf
            pthread_mutex_lock(&tcp_lock);
            ret = fprintf(network,"%s\r\n", monstring);
            pthread_mutex_unlock(&tcp_lock);

            if (ret <= 0) {

                fprintf(stderr,"Error communicating to server, %s:%s\n",igate_configuration.aprsishost,igate_configuration.aprsisport);
                fprintf(igate_configuration.logfile, "%s Exiting, error communicating to server, %s:%s.\n", format_gpstime(timebuffer,sizeof(timebuffer), gps_time_ns()), igate_configuration.aprsishost, igate_configuration.aprsisport);

                // error!
                fclose(network);
                network = NULL;
                //goto retry; // Try to reopen the network connection
                            
                // close the threads
                pthread_cancel(Read_thread);
                pthread_join(Read_thread,NULL);
                pthread_cancel(Beacon_thread);
                pthread_join(Beacon_thread,NULL);
                pthread_cancel(GPSD_thread);
                pthread_join(GPSD_thread,NULL);

                // break out of this inner loop
                break;
            }

        } // inner while loop
    } // outer while loop

    if (igate_configuration.logfile) {
        fprintf(igate_configuration.logfile, "%s Done.\n", format_gpstime(timebuffer, sizeof(timebuffer), gps_time_ns()));
        fclose(igate_configuration.logfile);
    }
}


// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// THREAD:  Just read and echo responses from the APRS-IS server
void *netreader(void *arg)
{
    pthread_setname("aprs-read");

    char timebuffer[1024];
    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    int num = 0;
    bool beacon_locked = true;

    // Create our own stream; there seem to be problems sharing a common stream among threads
    FILE *network = fdopen(Network_fd,"r");

    // Loop continuously as long as we're getting data from the APRS-IS servers
    while((linelen = getline(&line,&linecap,network)) > 0) {

        // increment our counter
        num++;

        // right trim off any newline/carrage return/spaces from the line we read from the APRS-IS server and place '\0' char at that location.
        char *end = line + strlen(line) -1;
        while (end > line && isspace(*end)) {
            end--;
        }
        *(end+1) = '\0';

        fprintf(igate_configuration.logfile, "%s %s: %s\n", format_gpstime(timebuffer, sizeof(timebuffer), gps_time_ns()), igate_configuration.aprsishost, line);
        //fwrite(line,linelen,1,igate_configuration.logfile);

        // if more 2 lines have been received from the APRS-IS server, then we signal the writing thread that its clear to xmit
        if (num > 1 && beacon_locked) {
            pthread_cond_signal(&cleartoxmit);
            beacon_locked = false;
        }
    }

    FREE(line);
    return NULL;
}


// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// Shutdown the application.  Should be called from kill signals.
void closedown(int x)
{
    char timebuffer[1024];

    if (Verbose)
        fprintf(igate_configuration.logfile, "%s Stopping app.  Received signal: %d\n", format_gpstime(timebuffer,sizeof(timebuffer), gps_time_ns()), x);

    // Set the global to true so that processing loops close down
    stop_processing = true;

}


// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// read the telemetry sequence from the telemetry sequence file
int readtelemseq(char *filename)
{
    FILE *telemfile;
    int i = 0;

    // Read in the last sequence from the telemetry file
    if (filename) {
        telemfile = fopen(filename, "r");

        char *line = NULL;
        size_t linecap = 0;
        ssize_t linelen;

        // read in the first line from the telemetry file
        if (telemfile) {
            linelen = getline(&line, &linecap, telemfile);

            // if we got something back, then we convert that to an integer and use that value as our starting sequence number.
            if (linelen > 0) {
                i = atoi(line);

                // check for rollover or oddness
                if (i > 999 || i < 0)
                    i = 0;
            }

            // close the file (ending sequence number is written to the telemfile upon application end)
            fclose(telemfile);
        }
        FREE(line);
    }

    return i;
}

// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// write the telemetry sequence to the telemetry sequence file
void writetelemseq(char *filename, int seq)
{
    FILE *telemfile;

    // write the sequence provide to the file.  This will overwrite the file, btw.
    if (filename) {
        telemfile = fopen(filename, "w");
        if (telemfile) {
            fprintf(telemfile, "%d\n", seq);
            fclose(telemfile);
        }
        else {
            fprintf(stderr, "Unable to write telemetry sequence, %d, to filename, %s.  Errno: %d\n", seq, filename, errno);
            stop_processing = true;
        }
    }
}

// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// Construct a packet string for beaconing our position, symbol, and comment.
int positpacket(char *buffer, size_t n, char *info)
{
    // sanity check
    if (buffer == NULL)
        return 0;

    time_t t = time(NULL);
    struct tm *ts = gmtime(&t);
    int hours = ts->tm_hour;
    int minutes = ts->tm_min;
    int seconds = ts->tm_sec;

    return snprintf (buffer, n, "%s>%s,TCPIP*:/%02d%02d%02dh%s", igate_configuration.callsign, tocall, hours, minutes, seconds, info);

}


// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// construct the telemetry report packet
int telemetrypacket(char *buffer, size_t n, APRS_EQNS *eqns)
{
    int num = 0;
    int received;
    int dropped;
    int received_sat;
    int heard_direct;
    int adjusted_receive;
    int adjusted_dropped;
    int adjusted_receive_sat;
    int adjusted_direct;
    double pct_dropped;
    double pct_direct;

    // sanity check
    if (buffer == NULL)
        return 0;

    // lock on the stats mutex
    pthread_mutex_lock(&stats_lock);

    // save the received and dropped packet values to local variables, then reset them back to zero
    received = aprs_statistics.received;
    dropped = aprs_statistics.dropped;
    received_sat = aprs_statistics.satellite;
    heard_direct = aprs_statistics.direct;
    aprs_statistics.received = 0;
    aprs_statistics.dropped = 0;
    aprs_statistics.satellite = 0;
    aprs_statistics.direct = 0;

    // unlock the stats mutex
    pthread_mutex_unlock(&stats_lock);

    // calculate the % dropped and direct if there were packets we received over the interval
    if (received > 0) {

        // calculate the percentages
        pct_dropped = 100.0 * (double) dropped / (double) received;
        pct_direct = 100.0 * (double) heard_direct / (double) received;

        // sanity check if the percentages are < 0, then just set to zero
        if (pct_dropped < 0)
            pct_dropped = 0.0;
        if (pct_direct < 0)
            pct_direct = 0.0;
    }
    else {
        // no received packets we just set the percentages to 0
        pct_dropped = 0.0;
        pct_direct = 0.0;
    }


    // Determine coefficients for the APRS equations packet.  We do this because telemetry values can only range from 0-255.
    adjusted_receive = calculatecoef(received, &(eqns->rx));
    adjusted_receive_sat = calculatecoef(received_sat, &(eqns->rxsat));
    adjusted_dropped = calculatecoef(pct_dropped, &(eqns->drp));
    adjusted_direct = calculatecoef(pct_direct, &(eqns->rxdirect));

    // write the telemetry packet string to the supplied buffer.
    num = snprintf(buffer, n, "%s>%s,TCPIP*:T#%03d,%03d,%03d,%03d,%03d,%03d,00000000,Telemetry report", 
            igate_configuration.callsign, 
            tocall, 
            igate_configuration.sequence, 
            adjusted_receive, 
            adjusted_receive_sat, 
            adjusted_dropped, 
            adjusted_direct, 
            0);

    // check the sequence number.  If > 999, then we roll it back to 000.
    igate_configuration.sequence++;
    if (igate_configuration.sequence > 999 || igate_configuration.sequence < 0)
        igate_configuration.sequence = 0;

    // write the last telemetry sequence to the telemetry sequence file
    writetelemseq(igate_configuration.telemseqfilename, igate_configuration.sequence);

    return num;

}


// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// for a given set of station details, fill a buffer with the constructed information string (for use as part of an APRS packet)
int createinfostring(char *buffer, size_t n, LOCATION *station)
{
    float local_lat, local_lon, lat_ms, lon_ms;
    int lat_d, lon_d, infosize, num;
    char lat_ns;
    char lon_ew;
    char lat_string[20];
    char lon_string[20];
    char csespd[8]; // only if this station mobile (ex. ismobile = true)
                    
    // convert the lat/lon to degrees, decimal minutes
    if (station->lat >= 0)
        lat_ns = 'N';
    else
        lat_ns = 'S';

    if (station->lon >= 0)
        lon_ew = 'E';
    else
        lon_ew = 'W';

    // remove any negative degrees
    local_lat = (station->lat < 0 ? -station->lat : station->lat);
    local_lon = (station->lon < 0 ? -station->lon : station->lon);

    // just the degrees (truncate off the decimal portion)
    lat_d = (int) local_lat;
    lon_d = (int) local_lon;

    // the decimal minutes
    lat_ms = (local_lat - lat_d) * 60;
    lon_ms = (local_lon - lon_d) * 60;

    // the latitude and longitude strings
    memset(lat_string, 0, sizeof(lat_string));
    memset(lon_string, 0, sizeof(lon_string));

    // For APRS, the position report represents latitude as ddmm.ssN or ddmm.ssS
    // For APRS, the position report represents longitude as dddmm.ssWor dddmm.ssE
    snprintf(lat_string, sizeof(lat_string), "%02d%05.2f%c", lat_d, lat_ms, lat_ns);
    snprintf(lon_string, sizeof(lon_string), "%03d%05.2f%c", lon_d, lon_ms, lon_ew);


    // construct the course/speed string
    memset(csespd, 0, sizeof(csespd));
    if (igate_configuration.ismobile) {
        double c = round(station->course);
        double s = round(station->speed * 10);

        // only if this station is moving at > 1 MPH
        if (s > 0) {
            snprintf (csespd, sizeof(csespd), "%03.0f/%03.0f", c, s);
        }
        else 
            snprintf (csespd, sizeof(csespd), "000/000");
    }

    // Determine if the buffer is shorter than the maximum size for a position report packet
    if (n > MAX_POSIT)
        infosize = MAX_POSIT;
    else
        infosize = n;


    // handle the symbol/overlay BS'ery with APRS
    char sym[2];
    char ovl[2];

    // clear the buffers
    sym[0] = sym[1] = ovl[0] = ovl[1] = '\0';

    // is the first character of the symbol the primary or alternate table identifier?
    if (strncmp(igate_configuration.symbol, "/", 1) == 0) {
        // primary table selected
        sym[0] = igate_configuration.symbol[1];
        ovl[0] = '/';
    }
    else if (strncmp(igate_configuration.symbol, "\\", 1) == 0) {
        // alternate table selected
        sym[0] = igate_configuration.symbol[1];
        if (igate_configuration.overlay[0] != '\0')
            ovl[0] = igate_configuration.overlay[0];
        else
            ovl[0] = '\\';
    }
    else {
        // just default to a red dot
        sym[0] = '/';
        ovl[0] = '/';
    }
    
    // Create the information string, saved to the buffer.  If the comment string is too long, then that is truncated at the 'infosize' mark.
    num = snprintf(buffer, infosize, "%s%s%s%s%s/A=%06d%s", lat_string, ovl, lon_string, sym, csespd, (int) station->alt, igate_configuration.comment);

    // return the number of bytes written to the buffer
    return num;
}



// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// calculate the APRS coefficients for the equations packet of the "value" parameter
int calculatecoef(double value, APRS_COEF *c) {
    int adjusted_value;
    double one_million = 1000000.0; // used for rounding coefficient values to 6 digits.

    // if the value is between -255 and +255 then we forego using the "a" coefficient
    if (value >= -255 && value <= 255) {
        adjusted_value = (value >= 0 ? (int) floor(value) : (int) ceil(value));
        c->a = 0;
        c->b = 1;
        c->c = round((value - adjusted_value) * one_million) / one_million;
    }
    else { // the value was < -255 or > +255...

        // just picking some base value for x
        adjusted_value = 128;

        // our calculated coefficient values and some remainder placeholders
        int A, B;
        double C, A_remainder, B_remainder;

        // if the value is positive
        if (value > 0) {
            // a coefficient (just making this an integer)
            A = (int) floor(value / (adjusted_value * adjusted_value));
            A_remainder = value - A*adjusted_value*adjusted_value;

            // b coefficient (just making this an integer)
            B = (int) floor(A_remainder / (adjusted_value));
            B_remainder = A_remainder - B*adjusted_value;

            // c coefficient...everything left over.  ;)
            C = B_remainder;
        }
        else { // if the value is negative

            // a coefficient (just making this an integer)
            A = (int) ceil(value / (adjusted_value * adjusted_value));
            A_remainder = value - A*adjusted_value*adjusted_value;

            // b coefficient (just making this an integer)
            B = (int) ceil(A_remainder / (adjusted_value));
            B_remainder = A_remainder - B*adjusted_value;

            // c coefficient...everything left over.  ;)
            C = B_remainder;
        }

        // round these to 6 digits.
        c->a = round(A * one_million) / one_million;
        c->b = round(B * one_million) / one_million;
        c->c = round(C * one_million) / one_million;
    }

    return adjusted_value;
}

// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// construct the APRS equation coefficents packet
int eqnpacket(char *buffer, size_t n, APRS_EQNS *eqns)
{
    if (buffer == NULL)
        return 0;

    //return snprintf(buffer, n, "%s>%s,TCPIP*::%-9s:EQNS.%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,0,0,0", 
    return snprintf(buffer, n, "%s>%s,TCPIP*::%-9s:EQNS.%g,%g,%g,%g,%g,%g,%g,%g,%g,%g,%g,%g,0,0,0", 
            igate_configuration.callsign, tocall, igate_configuration.callsign, 
            eqns->rx.a, eqns->rx.b, eqns->rx.c, 
            eqns->rxsat.a, eqns->rxsat.b, eqns->rxsat.c, 
            eqns->drp.a, eqns->drp.b, eqns->drp.c, 
            eqns->rxdirect.a, eqns->rxdirect.b, eqns->rxdirect.c 
            );

}

// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// construct the APRS parameter name packet
int parampacket(char *buffer, size_t n)
{
    if (buffer == NULL)
        return 0;

    return snprintf(buffer, n, "%s>%s,TCPIP*::%-9s:PARM.Rx10m,RxSat10m,PctDrop10m,PctRxDirect10m", igate_configuration.callsign, tocall, igate_configuration.callsign);

}

// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// construct the APRS units packet
int unitspacket(char *buffer, size_t n)
{
    if (buffer == NULL)
        return 0;

    return snprintf(buffer, n, "%s>%s,TCPIP*::%-9s:UNIT.Pkts,Pkts,%%,%%", igate_configuration.callsign, tocall, igate_configuration.callsign);

}

// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// construct the APRS bitsense packet
int bitspacket(char *buffer, size_t n)
{
    if (buffer == NULL)
        return 0;

    return snprintf(buffer, n, "%s>%s,TCPIP*::%-9s:BITS.00000000,KA9Q-radio Telemetry", igate_configuration.callsign, tocall, igate_configuration.callsign);

}

// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// THREAD:  send a position packet to the APRS-IS server
void *beaconthread(void *arg)
{
    pthread_setname("beacon-thread");

    // Buffer where we save the position packet that we "might" beacon to the APRS-IS server.
    char packet_string[332]; // the maximum size of an AX.25 UI frame is 332 bytes.
    char info_string[256];  // max size of the APRS information field
    int ret = 1;

    // The maximum amount of time (in seconds) we wait for before sending position packets to the APRS-IS server.
    int posit_max = 120;

    // we only want to send a set of telemetry packets every 10mins or so.  
    int telemetry_max = 600;

    // last xmit counters
    int elapsed_posit = posit_max - 20;
    int elapsed_telemetry = 0;

    // xmit flags
    bool xmit_telemetry = false;
    bool xmit_posit = false;

    // In the case of GPSD, we want to track the position of this station the last time we transmitted
    double last_lat = 0;
    double last_lon = 0;
    double last_alt = 0;

    // buffers 
    char timestring[1024];
    char eqns[256];
    char params[256];
    char units[256];
    char bits[256];
    char telem[256];
    APRS_EQNS aprs_eqns;

    // Create our own stream; there seem to be problems sharing a common stream among threads
    FILE *network = fdopen(Network_fd,"w");
    setlinebuf(network);

    // We don't want to conenct to the APRS-IS server until our connection to that server was successful.  So we wait until the reader thread has received confirmation from the APRS-IS server and has 
    // signaled that we can continue.
    
    // Attempt to get the beacon_lock that signifies that we can proceed with transmitting our beacons to the APRS-IS servers
    pthread_mutex_lock(&beacon_lock);

    // ..and now wait for the signal from the reader thread on if we can proceed or not...
    pthread_cond_wait(&cleartoxmit, &beacon_lock);

    // unlock as we're now clear to xmit to the APRS-IS server
    pthread_mutex_unlock(&beacon_lock);

    // Loop until we can't send data (i.e. something went wrong when talking to the APRS-IS server) or we've been told to stop
    while(ret > 0 && !stop_processing) {

        // set the transmit flags
        xmit_telemetry = false;
        xmit_posit = false;
        
        if (igate_configuration.gpsdenabled) {

            // lock the location structure to determine if our position has changed
            pthread_mutex_lock(&location_lock);
            if (location.gpsmode >= 3 && (((location.lat - last_lat > 0.0001 || location.lon - last_lon > 0.0001 || location.alt - last_alt > 100) && elapsed_posit > posit_max / 4) || elapsed_posit > posit_max)) {
                last_lat = location.lat;
                last_lon = location.lon;
                last_alt = location.alt;

                // initialize our info_string buffer
                memset(info_string, 0, sizeof(info_string));

                // create the information field for this position packet
                if (createinfostring(info_string, sizeof(info_string), &location) > 0) {

                    // initialize out packet buffer
                    memset(packet_string, 0, sizeof(packet_string));

                    // create a new position packet that we can send to the APRS-IS server
                    positpacket(packet_string, sizeof(packet_string), info_string);

                    // set the xmit flag for position packets.
                    xmit_posit = true;
                }
            }

            // unlock 
            pthread_mutex_unlock(&location_lock);

        }
        else {  // not GPSD enabled, so we'll just look to periodically transmit the static position of this station

            // has it been long enough?
            if (elapsed_posit > posit_max) {

                // initialize our info_string buffer
                memset(info_string, 0, sizeof(info_string));

                // create the information field for this position packet
                if (createinfostring(info_string, sizeof(info_string), &location) > 0) {

                    // initialize out packet buffer
                    memset(packet_string, 0, sizeof(packet_string));

                    // create a new position packet that we can send to the APRS-IS server
                    positpacket(packet_string, sizeof(packet_string), info_string);

                    // set the xmit flag for position packets.
                    xmit_posit = true;
                }
            }
        }

        // has it been long enough?  ..and it's now time to send a set of telemetry packets the APRS-IS server
        // For telemetry packets it's simple as we just need to wait a specific amount of time before sending.
        if (elapsed_telemetry > telemetry_max) {

            // construct telemetry packets
            telemetrypacket(telem, sizeof(telem), &aprs_eqns);
            eqnpacket(eqns, sizeof(eqns), &aprs_eqns);
            parampacket(params, sizeof(params));
            unitspacket(units, sizeof(units));
            bitspacket(bits, sizeof(bits));

            xmit_telemetry = true;
        }

        // lock the connection to the APRS-IS server.
        if (xmit_posit || xmit_telemetry)
            pthread_mutex_lock(&tcp_lock);


        if (xmit_posit) {

            // Send our position packet to the APRS-IS server
            fprintf(igate_configuration.logfile, "%s xmitting packet: %s\n", format_gpstime(timestring,sizeof(timestring), gps_time_ns()), packet_string);
            ret = fprintf(network, "%s\r\n", packet_string);

            // reset elapsed time since last transmission.
            elapsed_posit = 0;
        }

        if (ret && xmit_telemetry) {

            fprintf(igate_configuration.logfile, "%s xmitting packet: %s\n", format_gpstime(timestring,sizeof(timestring), gps_time_ns()), telem);
            fprintf(igate_configuration.logfile, "%s xmitting packet: %s\n", format_gpstime(timestring,sizeof(timestring), gps_time_ns()), eqns);
            fprintf(igate_configuration.logfile, "%s xmitting packet: %s\n", format_gpstime(timestring,sizeof(timestring), gps_time_ns()), params);
            fprintf(igate_configuration.logfile, "%s xmitting packet: %s\n", format_gpstime(timestring,sizeof(timestring), gps_time_ns()), units);
            fprintf(igate_configuration.logfile, "%s xmitting packet: %s\n", format_gpstime(timestring,sizeof(timestring), gps_time_ns()), bits);

            // send telemetry packets to the APRS-IS server
            if (ret)
                ret = fprintf(network, "%s\r\n", telem);
            if (ret)
                ret = fprintf(network, "%s\r\n", eqns);
            if (ret)
                ret = fprintf(network, "%s\r\n", params);
            if (ret)
                ret = fprintf(network, "%s\r\n", units);
            if (ret)
                ret = fprintf(network, "%s\r\n", bits);

            // reset the telemetry elapsed counter
            elapsed_telemetry = 0;
        }

        // unlock the connection to the APRS-IS server.
        if (xmit_posit || xmit_telemetry)
            pthread_mutex_unlock(&tcp_lock);

        // sleep for some small number of seconds
        if (ret) {
            struct timespec tv;
            tv.tv_sec = 10;  // 10 seconds
            tv.tv_nsec = 0;
            nanosleep(&tv, NULL);

            // increment counters
            elapsed_posit += 10;
            elapsed_telemetry += 10;
        }
        else 
            fprintf(stderr, "Error sending packet(s) to the APRS-IS server.\n");
    }

    return NULL;
}


// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// THREAD:  read from GPSD out posotion and populate global location structure.  Only used if gpsdenabled was set to true in the JSON configuration file.
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

        if (igate_configuration.gpsdenabled)
            gpshost = igate_configuration.gpsdhost;

        if (0 != gps_open(gpshost, "2947", &gps_data)) {
            fprintf(igate_configuration.logfile, "%s Unable to connect to GPSD on %s.  trycount=%d\n", format_gpstime(timebuffer, sizeof(timebuffer), gps_time_ns()), gpshost, trycount);

            // sleep for a few microseconds.  Ramping up the sleep time as the trycount gets large (i.e. we've lost connectivity all together...no sense in just continually retrying).
            float usecs = 10 * exp(trycount);
            usleep(usecs);
        }
        else {
            fprintf(igate_configuration.logfile, "%s Connected to GPSD on %s.\n", format_gpstime(timebuffer, sizeof(timebuffer), gps_time_ns()), gpshost);
        }


        (void) gps_stream(&gps_data, WATCH_ENABLE | WATCH_JSON, NULL);
        
        double change_threshold = .0001;
        double prev_lat, prev_lon;
        prev_lat = prev_lon = 0;

        // Loop continuously reading from GPSD
        while (gps_waiting(&gps_data, 5000000) && !stop_processing) {
            if (-1 == gps_read(&gps_data, NULL, 0)) {
                fprintf(igate_configuration.logfile, "%s Unable to read from GPSD on %s.\n", format_gpstime(timebuffer, sizeof(timebuffer), gps_time_ns()), gpshost);
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


                    // Update the GPS lat/lon variables on the global location structure.
                    pthread_mutex_lock(&location_lock);
                    location.gpsmode = gps_data.fix.mode;
                    location.lat = gps_data.fix.latitude;
                    location.lon = gps_data.fix.longitude;
                    location.alt = gps_data.fix.altitude * 3.2808399; // converted to feet
                    location.speed = gps_data.fix.speed * 2.236936; // converted to mph
                    location.course = gps_data.fix.track;
                    pthread_mutex_unlock(&location_lock);

                }
                
                // reset the trycount since we've just read data from the GPS
                if (trycount > 0) {
                    fprintf(igate_configuration.logfile, "%s Reconnected to GPSD on %s.  GPS mode: %d.\n", format_gpstime(timebuffer, sizeof(timebuffer), gps_time_ns()), gpshost, gps_data.fix.mode);
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
    fprintf(igate_configuration.logfile, "%s GPSD thread ended.\n", format_gpstime(timebuffer, sizeof(timebuffer), gps_time_ns()));

    return NULL;
}


// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// This will parse the given JSON element array/object and look for configuration settings.  This is recursive.
void processJSONObject(cJSON *json) {
    int jsize = cJSON_GetArraySize(json);
    int i;
    cJSON *item;

    // loop through each JSON key value
    for (i = 0; i < jsize; i++) {

        // this particular JSON item
        item = cJSON_GetArrayItem(json, i);

        // make sure we're able to parse the key/value pair
        if (item == NULL) {
            fprintf(stderr, "Unable to get json item: %d\n", i);
            break;
        }
        else {

            // Determine if this key/value pair is actually another object or array.  If it is, we just call ourselves again...
            if (cJSON_IsObject(item) || cJSON_IsArray(item))
                processJSONObject(item);
            else // ...otherwise, it's an "item" so we check it against the configuration elements were looking for.
                checkForConfigurable(item);
        }
    }
}


// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// check the JSON item for a configuration item
void checkForConfigurable(cJSON *item) {
    if (cJSON_IsInvalid(item))
        return;

    if (strcmp(item->string, "ax25host") == 0)
        strncpy(igate_configuration.ax25host, item->valuestring, sizeof(igate_configuration.ax25host)-1);
    else if (strcmp(item->string, "callsign") == 0)
        strncpy(igate_configuration.callsign, item->valuestring, sizeof(igate_configuration.callsign)-1);
    else if (strcmp(item->string, "passcode") == 0) 
       snprintf(igate_configuration.passcode, sizeof(igate_configuration.passcode), "%d", item->valueint);
    else if (strcmp(item->string, "aprsishost") == 0)
        strncpy(igate_configuration.aprsishost, item->valuestring, sizeof(igate_configuration.aprsishost)-1);
    else if (strcmp(item->string, "aprsisport") == 0) {
        int val = (item->valuedouble > 0 && item->valuedouble < 99999 ? (int) item->valuedouble : 14580);
        snprintf (igate_configuration.aprsisport, sizeof(igate_configuration.aprsisport), "%d", val);
    }
    else if (strcmp(item->string, "aprsisbeaconing") == 0) 
        igate_configuration.aprsisbeaconing = (cJSON_IsTrue(item) ? true : false);
    else if (strcmp(item->string, "stationlat") == 0) 
        location.lat = item->valuedouble;
    else if (strcmp(item->string, "stationlon") == 0) 
        location.lon = item->valuedouble;
    else if (strcmp(item->string, "stationalt") == 0) 
        location.alt = item->valuedouble;
    else if (strcmp(item->string, "gpsdhost") == 0)
        strncpy(igate_configuration.gpsdhost, item->valuestring, sizeof(igate_configuration.gpsdhost)-1);
    else if (strcmp(item->string, "gpsdenabled") == 0) 
        igate_configuration.gpsdenabled = (cJSON_IsTrue(item) ? true : false);
    else if (strcmp(item->string, "usememcache") == 0) 
        igate_configuration.usememcache = (cJSON_IsTrue(item) ? true : false);
    else if (strcmp(item->string, "symbol") == 0)
        strncpy(igate_configuration.symbol, item->valuestring, sizeof(igate_configuration.symbol)-1);
    else if (strcmp(item->string, "overlay") == 0)
        strncpy(igate_configuration.overlay, item->valuestring, sizeof(igate_configuration.overlay)-1);
    else if (strcmp(item->string, "comment") == 0)
        strncpy(igate_configuration.comment, item->valuestring, sizeof(igate_configuration.comment)-1);
    else if (strcmp(item->string, "ismobile") == 0) 
        igate_configuration.ismobile = (cJSON_IsTrue(item) ? true : false);
    else if (strcmp(item->string, "telemseqfilename") == 0)
        strncpy(igate_configuration.telemseqfilename, item->valuestring, sizeof(igate_configuration.telemseqfilename)-1);
}


// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// initialize location structure
void initlocation(LOCATION *loc) {
    loc->lat = 0.0;
    loc->lon = 0.0;
    loc->alt = 0.0;
    loc->course = 0.0;
    loc->speed = 0.0;
    loc->gpsmode = 0;
}


// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// initialize packet statistics structure
void initpacketstatistics(PACKET_STATISTICS *stats) {
    stats->dropped = 0;
    stats->received = 0;
    stats->satellite = 0;
    stats->direct = 0;
}


// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// initialize igate configuration structure
void initigateconfig(IGATE_CONFIGURATION *config) {
    config->ismobile = false;
    memset(config->gpsdhost, 0, sizeof(config->gpsdhost));
    config->gpsdenabled = false;
    config->usememcache = false;
    snprintf(config->ax25host, sizeof(config->ax25host), "ax25.local");
    snprintf(config->aprsishost, sizeof(config->aprsishost), "noam.aprs2.net");
    snprintf(config->aprsisport, sizeof(config->aprsisport), "14580");
    config->aprsisbeaconing = false;
    memset(config->telemseqfilename, 0, sizeof(config->telemseqfilename));
    config->sequence = 0;
    memset(config->logfilename, 0, sizeof(config->logfilename));
    memset(config->callsign, 0, sizeof(config->callsign));
    memset(config->passcode, 0, sizeof(config->passcode));
    memset(config->symbol, 0, sizeof(config->symbol));
    memset(config->overlay, 0, sizeof(config->overlay));
    memset(config->comment, 0, sizeof(config->comment));
    memset(config->configfilename, 0, sizeof(config->configfilename));
    config->logfile = NULL;
    config->configfile = NULL;
}



// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// determine the APRS passcode for a given callsign.  returns 0 if there was a failure, otherwise return the number of characters copied to the buffer.
int passcode(char *call, char *buffer, size_t n_buffer) {

    // Calculate trivial hash authenticator
    int hash = 0x73e2;
    char callsign[16]; // limit the callsign characters we'll look at to only 16 chars.  Shouldn't be an issue as ham callsigns are limited to 8 chars (i.e. AB0DEF-99).
    char *cp;

    // make a copy of the incoming callsign, 'call', and save that into a local array for manipulation - so we don't muck with the original.
    strlcpy(callsign, call, sizeof(callsign));

    // find the hyphen in the callsign string...if found, replace that character with a \0.
    if((cp = strchr(callsign,'-')) != NULL)
        *cp = '\0';

    // how long is the callsign (without the extra hyphen and ssid)
    int const len = strlen(callsign);

    // loop through each character in the callsign, computing the running hash of each character
    for(int i=0; i<len; i += 2) {
        hash ^= toupper(callsign[i]) << 8;
        hash ^= toupper(callsign[i+1]);
    }

    // mask off only first couple of bytes (but not the most signficant bit)
    hash &= 0x7fff;

    // save the resulting passcode to the buffer
    return snprintf(buffer, n_buffer, "%d", hash);
}
