// Process AX.25 frames containing APRS data, feed to APRS2 network
// Copyright 2018-2023, Phil Karn, KA9Q

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
#include <sysexits.h>

#include "multicast.h"
#include "ax25.h"
#include "misc.h"

// structures for APRS telemetry data
// ---------------------------------------------------------------------
typedef struct {
    int a;
    int b;
    int c;
} APRS_COEF;

typedef struct {
    APRS_COEF rx;
    APRS_COEF drp;
    APRS_COEF rxsat;
} APRS_EQNS;
// ---------------------------------------------------------------------


// global variables
// ---------------------------------------------------------------------
char *Mcast_address_text = "ax25.local";
char *Host = "noam.aprs2.net";
char *Port = "14580";
char *User;
char *Passcode;
char *Logfilename;
char *Latitude;
char *Longitude;
char *Altitude;
char *Comment;

// By default, when beaconing to APRS-IS, we represent this station as an Rx-only Igate.
char *Symbol = "&";
char *Overlay = "R";

// for keeping track of the number of received and dropped (because of bad stuff) packets
int dropped_pkts = 0;
int received_pkts = 0;
int received_sat_pkts = 0;

// APRS telemetry sequence number
int sequence = 0;

// APRS tocall value.  Experimental tocalls start with APZxxx.  Using "KR1" for "KA9Q-Radio 1".  Dunno...just making this up.  ;)
char *tocall = "APZKR1";

// By default beaconing to APRS-IS is not enabled.
bool beaconing_enabled = false;
char info_string[128];

FILE *Logfile;
const char *App_path;
int Verbose;

int Input_fd = -1;
int Network_fd = -1;
// ---------------------------------------------------------------------

// Thread mutex, conditions, and threads
// ---------------------------------------------------------------------
pthread_mutex_t tcp_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t beacon_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cleartoxmit = PTHREAD_COND_INITIALIZER;
pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;

pthread_t Beacon_thread;
pthread_t Read_thread;
// ---------------------------------------------------------------------


// Functions
// ---------------------------------------------------------------------
void *netreader(void *arg);
void *beaconthread(void *arg);
int positpacket(char *buffer, size_t n);
int bitspacket(char *buffer, size_t n);
int unitspacket(char *buffer, size_t n);
int parampacket(char *buffer, size_t n);
int eqnpacket(char *buffer, size_t n, APRS_EQNS *eqns);
int calculatecoef(int value, APRS_COEF *c);
int telemetrypacket(char *buffer, size_t n, APRS_EQNS *eqns);
// ---------------------------------------------------------------------


// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// main function
// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
int main(int argc,char *argv[])
{
    App_path = argv[0];
    // Quickly drop root if we have it
    // The sooner we do this, the fewer options there are for abuse
    if(seteuid(getuid()) != 0)
        fprintf(stderr,"seteuid: %s\n",strerror(errno));

    setlocale(LC_ALL,getenv("LANG"));
    setlinebuf(stdout);

    int c;
    while((c = getopt(argc,argv,"u:p:I:vh:f:L:G:A:S:O:C:V:")) != EOF) {
        switch(c) {
            case 'f':
                Logfilename = optarg;
                Verbose = 0;
                break;
            case 'u':
                User = optarg;
                break;
            case 'v':
                if(!Logfilename)
                    Verbose++;
                break;
            case 'h':
                Host = optarg;
                break;
            case 'p':
                Passcode = optarg;
                break;
            case 'I':
                Mcast_address_text = optarg;
                break;
            case 'L':
                Latitude = optarg;
                break;
            case 'G':
                Longitude = optarg;
                break;
            case 'A':
                Altitude = optarg;
                break;
            case 'C':
                Comment = optarg;
                break;
            case 'S':
                Symbol = optarg;
                break;
            case 'O':
                Overlay = optarg;
                break;
            case 'V':
                VERSION();
                exit(EX_OK);
            default:
                fprintf(stderr,"Usage: %s -u user [-p passcode] [-v] [-I mcast_address][-h host] [-L latitude] [-G longitude] [-A altitude] [-C comment string] [-S symbol char] [-O overlay char]\n",argv[0]);
                exit(EX_USAGE);
        }
    }


    // Set up multicast input
    {
        struct sockaddr_storage sock;
        resolve_mcast(Mcast_address_text,&sock,DEFAULT_RTP_PORT,NULL,0);
        Input_fd = listen_mcast(&sock,NULL);
    }
    if(Input_fd == -1) {
        fprintf(stderr,"Can't set up multicast input from %s\n",Mcast_address_text);
        exit(EX_IOERR);
    }

    if(Logfilename)
        Logfile = fopen(Logfilename,"a");
    else if(Verbose)
        Logfile = stdout;

    if(Logfile) {
        setlinebuf(Logfile);
        fprintf(Logfile,"APRS feeder program by KA9Q\n");
    }

    if(User == NULL) {
        fprintf(stderr,"Must specify -u User\n");
        exit(EX_USAGE);
    }


    if(!Passcode) {
        // Calculate trivial hash authenticator
        int hash = 0x73e2;
        char callsign[11];
        strlcpy(callsign,User,sizeof(callsign));
        char *cp;
        if((cp = strchr(callsign,'-')) != NULL)
            *cp = '\0';

        int const len = strlen(callsign);

        for(int i=0; i<len; i += 2) {
            hash ^= toupper(callsign[i]) << 8;
            hash ^= toupper(callsign[i+1]);
        }
        hash &= 0x7fff;
        if(asprintf(&Passcode,"%d",hash) < 0) {
            fprintf(stderr,"Unexpected error in computing passcode\n");
            exit(EX_SOFTWARE);
        }
    }

    // print out particulars about the command line arguments used and determine if we're beaconing
    if (Latitude && Longitude && Altitude && Symbol && Overlay && Comment) {

        // print out some info
        fprintf(Logfile, "%s invoked with latitude=%s, longitude=%s, altitude=%s, symbol=%s, overlay=%s, comment=%s\n", argv[0], Latitude, Longitude, Altitude, Symbol, Overlay, Comment);

        // convert values to floating points
        float lat = atof(Latitude);
        float lon = atof(Longitude);
        int alt = atoi(Altitude);

        fprintf(Logfile, "Location of this station: %.4f, %.4f at %d\n", lat, lon, alt);

        // convert the lat/lon to degrees, decimal minutes
        char lat_ns;
        char lon_ew;
        if (lat >= 0)
            lat_ns = 'N';
        else
            lat_ns = 'S';

        if (lon >= 0)
            lon_ew = 'E';
        else
            lon_ew = 'W';

        // remove any negative degrees
        lat = (lat < 0 ? -lat : lat);
        lon = (lon < 0 ? -lon : lon);

        // just the degrees
        int lat_d = (int) lat;
        int lon_d = (int) lon;

        // the decimal minutes
        float lat_ms = (lat - lat_d) * 60;
        float lon_ms = (lon - lon_d) * 60;

        // the latitude and longitude strings
        char lat_string[20];
        char lon_string[20];
        memset(lat_string, 0, sizeof(lat_string));
        memset(lon_string, 0, sizeof(lon_string));
        snprintf(lat_string, sizeof(lat_string), "%02d%05.2f%c", lat_d, lat_ms, lat_ns);
        snprintf(lon_string, sizeof(lon_string), "%03d%05.2f%c", lon_d, lon_ms, lon_ew);

        // Create the position string used in the posit beacon text.
        // for APRS, the position report represents latitude as ddmm.ssN or ddmm.ssS
        // for APRS, the position report represents longitude as dddmm.ssWor dddmm.ssE
        memset(info_string, 0, sizeof(info_string));
        int num = snprintf(info_string, sizeof(info_string), "%s%s%s%s/A=%06d%s", lat_string, Overlay, lon_string, Symbol, alt, Comment);

        if (num > 128) {
            beaconing_enabled = false;
            fprintf(stderr, "Position string for APRS-IS beaconing was too long:  %s\n", info_string);
            fprintf(stderr, "APRS-IS beaconing disabled.\n");
        } else {

            // Finally we enable beaconing if all the above variable have been properly set.
            beaconing_enabled = true;

        }
    }


    while(true) {
        // Resolve the APRS network server
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
            if((ecode = getaddrinfo(Host,Port,&hints,&results)) == 0)
                break;
            usleep(500000); // 500 ms
        }
        if(ecode != 0) {
            fprintf(stderr,"Can't getaddrinfo(%s,%s): %s\n",Host,Port,gai_strerror(ecode));
            usleep(5000000); // 5 sec
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
            fprintf(stderr,"Can't connect to server %s:%s\n",Host,Port);
            sleep(600); // 5 minutes
            freeaddrinfo(results);
            resp = results = NULL;
            continue;
        }
        if(Logfile)
            fprintf(Logfile,"Connected to APRS server %s port %s\n",resp->ai_canonname,Port);

        freeaddrinfo(results);
        resp = results = NULL;

        FILE *network = fdopen(Network_fd,"w+");
        setlinebuf(network);

        pthread_create(&Read_thread,NULL,netreader,NULL);
        pthread_create(&Beacon_thread,NULL,beaconthread,NULL);

        // Log into the network
        if(fprintf(network,"user %s pass %s vers KA9Q-aprs 1.0\r\n",User,Passcode) <= 0) {
            // error
            fclose(network);
            network = NULL;
            sleep(600); // 5 minutes;
            continue;
        }
        uint8_t packet[PKTSIZE];
        int size;
        while((size = recv(Input_fd,packet,sizeof(packet),0)) > 0) {
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

            if(Logfile) {
                // Emit local timestamp
                char result[1024];

                fprintf(Logfile,"%s ssrc %u seq %d",
                        format_gpstime(result,sizeof(result),gps_time_ns()),
                        rtp_header.ssrc,rtp_header.seq);
            }

            // Parse incoming AX.25 frame
            struct ax25_frame frame;
            if(ax25_parse(&frame,dp,size) < 0) {
                if(Logfile)
                    fprintf(Logfile," Unparsable packet\n");

                pthread_mutex_lock(&stats_lock);
                dropped_pkts++;
                pthread_mutex_unlock(&stats_lock);

                continue;
            }

            // Construct TNC2-style monitor string for APRS reporting
            char monstring[2048]; // Should be large enough for any legal AX.25 frame; we'll assert this periodically
            int sspace = sizeof(monstring);
            int infolen = 0;
            int is_tcpip = 0;
            {
                memset(monstring,0,sizeof(monstring));
                char *cp = monstring;
                {
                    int w = snprintf(cp,sspace,"%s>%s",frame.source,frame.dest);
                    cp += w;
                    sspace -= w;
                    assert(sspace > 0);
                }
                for(int i=0; i<frame.ndigi; i++) {
                    // if "TCPIP" appears, this frame came off the Internet and should not be sent back to it
                    if(strcmp(frame.digipeaters[i].name,"TCPIP") == 0)
                        is_tcpip = 1;
                    int const w = snprintf(cp,sspace,",%s%s",frame.digipeaters[i].name,frame.digipeaters[i].h ? "*" : "");
                    cp += w;
                    sspace -= w;
                    assert(sspace > 0);
                }
                {
                    // qAR means a bidirectional i-gate, qAO means receive-only
                    //    w = snprintf(cp,sspace,",qAR,%s",User);
                    int const w = snprintf(cp,sspace,",qAO,%s",User);
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
            assert(sizeof(monstring) - sspace - 1 == strlen(monstring));
            if(Logfile)
                fprintf(Logfile," %s\n",monstring);

            if(frame.control != 0x03 || frame.type != 0xf0) {
                if(Logfile)
                    fprintf(Logfile," Not relaying: invalid ax25 ctl/protocol\n");

                pthread_mutex_lock(&stats_lock);
                dropped_pkts++;
                pthread_mutex_unlock(&stats_lock);

                continue;
            }
            if(infolen == 0) {
                if(Logfile)
                    fprintf(Logfile," Not relaying: empty I field\n");

                pthread_mutex_lock(&stats_lock);
                dropped_pkts++;
                pthread_mutex_unlock(&stats_lock);

                continue;
            }
            if(is_tcpip) {
                if(Logfile)
                    fprintf(Logfile," Not relaying: Internet relayed packet\n");

                pthread_mutex_lock(&stats_lock);
                dropped_pkts++;
                pthread_mutex_unlock(&stats_lock);

                continue;
            }
            if(frame.information[0] == '{') {
                if(Logfile)
                    fprintf(Logfile," Not relaying: third party traffic\n");

                pthread_mutex_lock(&stats_lock);
                dropped_pkts++;
                pthread_mutex_unlock(&stats_lock);

                continue;
            }

            pthread_mutex_lock(&stats_lock);
            received_pkts++;
            if (rtp_header.ssrc == 145825)
                received_sat_pkts++;
            pthread_mutex_unlock(&stats_lock);

            int ret;
            // Send to APRS network with appended crlf
            pthread_mutex_lock(&tcp_lock);
            ret = fprintf(network,"%s\r\n",monstring);
            pthread_mutex_unlock(&tcp_lock);
            if (ret <= 0) {
                // error!
                fclose(network);
                network = NULL;
                goto retry; // Try to reopen the network connection
            }
        }
retry:
        ;
        pthread_cancel(Read_thread);
        pthread_join(Read_thread,NULL);
        pthread_cancel(Beacon_thread);
        pthread_join(Beacon_thread,NULL);
    }
}

// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// Just read and echo responses from server
void *netreader(void *arg)
{
    pthread_setname("aprs-read");

    // lock the beacon_lock
    //pthread_mutex_lock(&beacon_lock);

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

        // write all incoming data from the APRS-IS server to our log file
        if(Logfile)
            fwrite(line,linelen,1,Logfile);

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
// Construct a packet string for beaconing our position, symbol, and comment.
int positpacket(char *buffer, size_t n)
{
    if (buffer == NULL)
        return 0;

    time_t t = time(NULL);
    struct tm *ts = gmtime(&t);
    int hours = ts->tm_hour;
    int minutes = ts->tm_min;
    int seconds = ts->tm_sec;

    return snprintf (buffer, n, "%s>%s,TCPIP*:/%02d%02d%02dh%s", User, tocall, hours, minutes, seconds, info_string);
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
    int adjusted_receive;
    int adjusted_dropped;
    int adjusted_receive_sat;

    if (buffer == NULL)
        return 0;

    // get the received and dropped packet values, then reset them back to zero
    pthread_mutex_lock(&stats_lock);
    received = received_pkts;
    dropped = dropped_pkts;
    received_sat = received_sat_pkts;
    received_pkts = 0;
    dropped_pkts = 0;
    received_sat_pkts = 0;
    pthread_mutex_unlock(&stats_lock);

    // Determine coefficients for the APRS equations packet.  We do this because telemetry values can only range from 0-255.
    adjusted_receive = calculatecoef(received, &(eqns->rx));
    adjusted_dropped = calculatecoef(dropped, &(eqns->drp));
    adjusted_receive_sat = calculatecoef(received_sat, &(eqns->rxsat));

    // return the telemetry packet as a string
    num = snprintf(buffer, n, "%s>%s,TCPIP*:T#%03d,%03d,%03d,%03d,%03d,%03d,00000000,Telemetry report", User, tocall, sequence, adjusted_receive, adjusted_dropped, adjusted_receive_sat, 0, 0);

    // check the sequence number.  If > 999, then we roll it back to 000.
    sequence++;
    if (sequence > 999 || sequence < 0)
        sequence = 0;


    return num;

}

// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// calculate the APRS coefficients for the equations packet of the "value" parameter
int calculatecoef(int value, APRS_COEF *c) 
{
    int adjusted_value = value;

    if (value <= 255) {
        c->a = 0;
        c->b = 1;
        c->c = 0;
    }
    else {
        c->a = 0;
        c->b = value / 255;
        c->c = value % 255;
        adjusted_value = 255;
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

    return snprintf(buffer, n, "%s>%s,TCPIP*::%-9s:EQNS.%d,%d,%d,%d,%d,%d,%d,%d,%d,0,0,0,0,0,0", 
            User, tocall, User, eqns->rx.a, eqns->rx.b, eqns->rx.c, eqns->drp.a, eqns->drp.b, eqns->drp.c, eqns->rxsat.a, eqns->rxsat.b, eqns->rxsat.c);

}

// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// construct the APRS parameter name packet
int parampacket(char *buffer, size_t n)
{
    if (buffer == NULL)
        return 0;

    return snprintf(buffer, n, "%s>%s,TCPIP*::%-9s:PARM.Rx10m,Drop10m,RxSat10m", User, tocall, User);

}

// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// construct the APRS units packet
int unitspacket(char *buffer, size_t n)
{
    if (buffer == NULL)
        return 0;

    return snprintf(buffer, n, "%s>%s,TCPIP*::%-9s:UNIT.Pkts,Pkts,Pkts", User, tocall, User);

}

// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// construct the APRS bitsense packet
int bitspacket(char *buffer, size_t n)
{
    if (buffer == NULL)
        return 0;

    return snprintf(buffer, n, "%s>%s,TCPIP*::%-9s:BITS.00000000,KA9Q-radio Telemetry", User, tocall, User);

}

// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// send a position packet to the APRS-IS server
void *beaconthread(void *arg)
{
    pthread_setname("beacon-thread");

    char packet_string[256];
    int ret = 1;

    // This is the number of seconds we wait in between sending beacons to the APRS-IS server.
    int delta = 600;

    // Create our own stream; there seem to be problems sharing a common stream among threads
    FILE *network = fdopen(Network_fd,"w");
    setlinebuf(network);

    // Attempt to get the beacon_lock that signifies that we can proceed with transmitting our beacons to the APRS-IS servers
    pthread_mutex_lock(&beacon_lock);

    // ..and now wait for the signal from the reader thread on if we can proceed or not...
    pthread_cond_wait(&cleartoxmit, &beacon_lock);

    // unlock as we're now clear to xmit to the APRS-IS server
    pthread_mutex_unlock(&beacon_lock);

    // Loop until we can't send data to the APRS-IS server
    while(ret > 0) {

        // check how long it's been since we last sent a beacon to the APRS-IS server
        if (beaconing_enabled) {
            memset(packet_string, 0, sizeof(packet_string));
            if (positpacket(packet_string, sizeof(packet_string))) {
                char timestring[1024];
                char eqns[256];
                char params[256];
                char units[256];
                char bits[256];
                char telem[256];
                APRS_EQNS aprs_eqns;

                telemetrypacket(telem, sizeof(telem), &aprs_eqns);
                eqnpacket(eqns, sizeof(eqns), &aprs_eqns);
                parampacket(params, sizeof(params));
                unitspacket(units, sizeof(units));
                bitspacket(bits, sizeof(bits));

                //printf("%s\n", telem);
                //printf("%s\n", params);
                //printf("%s\n", units);
                //printf("%s\n", eqns);
                //printf("%s\n", bits);

                pthread_mutex_lock(&tcp_lock);
                fprintf(Logfile, "%s xmitting packet: %s\n", format_gpstime(timestring,sizeof(timestring), gps_time_ns()), packet_string);
                fprintf(Logfile, "%s xmitting packet: %s\n", format_gpstime(timestring,sizeof(timestring), gps_time_ns()), telem);
                fprintf(Logfile, "%s xmitting packet: %s\n", format_gpstime(timestring,sizeof(timestring), gps_time_ns()), eqns);
                fprintf(Logfile, "%s xmitting packet: %s\n", format_gpstime(timestring,sizeof(timestring), gps_time_ns()), params);
                fprintf(Logfile, "%s xmitting packet: %s\n", format_gpstime(timestring,sizeof(timestring), gps_time_ns()), units);
                fprintf(Logfile, "%s xmitting packet: %s\n", format_gpstime(timestring,sizeof(timestring), gps_time_ns()), bits);
                ret = fprintf(network, "%s\r\n", packet_string);
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
                pthread_mutex_unlock(&tcp_lock);

                if (ret <= 0 ) {
                    fprintf(stderr, "beacon write error\n");
                }
            }
        }

        // sleep for delta secs
        if (ret > 0) {
            sleep(delta);
        }

    }

    return NULL;
}
