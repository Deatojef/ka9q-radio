// db.c
//
// Functions for logging into, and writing power data to a database table.


#include <stdio.h>
#include <time.h>
#include <assert.h>
#include <stdlib.h>	
#include <string.h>	
#include <ctype.h>	
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>

// This is for postgresql
#ifdef __APPLE__
#include </opt/local/include/postgresql96/libpq-fe.h>
#else
#include <postgresql/libpq-fe.h>
#endif

#include "misc.h"
#include "db.h"


#define MAX_SAFE 500

static PGconn *db_connection;
char hostname[256];
char tablename[256];


/*------------------------------------------------------------------
 *
 * Function:	db_init
 *
 * Purpose:	Initialization at start of application for creating database connections.
 *
 * Inputs:	the misc_config struct that contains all of the parameters we're interested in.
 *
 * Global Out:	db_connection 	- PostgresQL database connection object.
 *
 *------------------------------------------------------------------*/



bool db_init (char *host, char *dbname, char *user, char *table, char *passfile)
{
    // hardcoding this for the moment just for testing.
    char connection_string[2048];
    char *host_string = host;
    char *user_string = user;
    char *dbname_string = dbname;

    bool okay = true;

    // get the system's hostname
    memset(hostname, 0, sizeof(hostname));
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        fprintf(stderr, "Unable to get system's hostname\n");
        strncpy(hostname, "localhost", sizeof(hostname));
    }

    // set the table name
    memset(tablename, 0, sizeof(tablename));
    strncpy(tablename, table, sizeof(tablename) - 1);

    //pgpass="monitor1:5432:powerdata:monitor1:thisisaboguspassword!"
    memset(connection_string, 0, sizeof(connection_string));
    snprintf(connection_string, sizeof(connection_string), "user=%s sslmode=require host=%s dbname=%s passfile=%s", user_string, host_string, dbname_string, passfile);

    // connect to the database...
    db_connection = PQconnectdb(connection_string);

    if (PQstatus(db_connection) != CONNECTION_OK) {
        fprintf (stderr, "Unable to connect to database, connection_string=%s, %s.\n", connection_string, PQerrorMessage(db_connection));
        PQfinish(db_connection);
        okay = false;
    } 
    else {
        //fprintf(stdout, "Connection to database, %s, successful.\n", dbname_string);
        //fprintf(stdout, "Database client_encoding status:  %s.\n", PQparameterStatus(db_connection, "client_encoding"));
        okay = true;
    }

    return(okay);
}


// timestamp, ssrc, lna_gain, mixer_gain, if_gain, input_power_dbfs, frontend_gain_db, baseband_power_db, snr_dB
bool db_write(long long timestamp, int gps_mode, double gps_lat, double gps_lon, int ssrc, int lna_gain, int mixer_gain, int if_gain, double input_power_dbfs, double frontend_gain_db, double baseband_power_db, double snr_db)
{
    // DB objects
    PGresult *res;
    ExecStatusType resultStatus;

    // the DB insert statement
    char insert_sql[1024];

    // time string buffer
    char timestring[256];

    // check the DB connection
    if (PQstatus(db_connection) != CONNECTION_OK) {
	    fprintf(stderr, "Connection to database failed:  %s.\n", PQerrorMessage(db_connection));
	    return false;
    }

    // remove trailing characters from the packet string
    //trim(packet_string);

    // clear out buffers
    memset(insert_sql, 0, sizeof(insert_sql));
    memset(timestring, 0, sizeof(timestring));

    // construct the time string
    format_gpstime(timestring, sizeof(timestring), timestamp);

    snprintf(insert_sql, sizeof(insert_sql), "insert into %s (time, host, gps_mode, gps_lat, gps_lon, frequency, lna_gain, mixer_gain, if_gain, input_power_dbfs, frontend_gain_db, baseband_power_db, snr_db) values ('%s', '%s', %d, %f, %f, %d, %d, %d, %d, %.2f, %.2f, %.2f, %.2f);", 
            tablename,
            timestring,
            hostname,
            gps_mode,
            gps_lat, 
            gps_lon,
            ssrc, 
            lna_gain, 
            mixer_gain,
            if_gain,
            input_power_dbfs,
            frontend_gain_db,
            baseband_power_db,
            snr_db
    );

    // Execute the SQL statement 
    res = PQexec(db_connection, insert_sql);
    resultStatus = PQresultStatus(res);

    // Check return code 
    if (resultStatus != PGRES_COMMAND_OK) {
        fprintf (stderr, "Error inserting row:  %s.\n", PQerrorMessage(db_connection));
        fprintf (stderr, "SQL:  %s.\n", insert_sql);
        return false;
    }

    //fprintf (stdout, "SQL:  %s.\n", insert_sql);

    return true;

}


/*------------------------------------------------------------------
 *
 * Function:	db_term
 *
 * Purpose:	Close any open DB connections
 *		Called when exiting.
 *
 *------------------------------------------------------------------*/

void db_term (void) {
    PQfinish(db_connection);
} // end db_term 



/*------------------------------------------------------------------
 *
 * Function:    make_ascii_only
 *
 * Purpose:     This function is almost exactly identical to ax25_safe_print
 *              except that instead of actually printing the resulting 
 *              string, it returns it.
 *
 * Inputs:      pstr    - Pointer to string we want to convert.
 *              
 *              len     - Number of bytes.  If < 0 we use strlen().
 *
 *              safe_str    - destination buffer where contents of the output will be placed.
 *
 *              dest_size   - size of destination buffer
 *
 *              
 *              Stops after non-zero len characters or at nul.
 *
 * Returns:     returns the print "safe" string
 *
 * Description: convert a string in a "safe" printable string.
 *
 * ------------------------------------------------------------------*/

char * make_ascii_only (char *pstr, int len, char *safe_str, int dest_size) {
    int ch;
    int safe_len;

    safe_len = 0;
    safe_str[safe_len] = '\0';

    if (len < 0)
        len = strlen(pstr);

    if (len > MAX_SAFE)
        len = MAX_SAFE;

    while (len > 0) {
        ch = *((unsigned char *)pstr);
        if (ch == ' ' && (len == 1 || pstr[1] == '\0')) {
            snprintf (safe_str + safe_len, dest_size - safe_len, "<0x%02x>", ch);
            safe_len += 6;
        }
        else if (ch < ' ' || ch >= 0x80 ) {
            snprintf (safe_str + safe_len, dest_size - safe_len, "<0x%02x>", ch);
            safe_len += 6;
        }
        else {
            // Everthing else is a printable char 
            safe_str[safe_len++] = ch;
            safe_str[safe_len] = '\0';
        }

        pstr++;
        len--;
    }

    return safe_str;

} // end make_ascii_only  


/*------------------------------------------------------------------
 * Trim any CR, LF from the end of line. 
 *
 *  ...borrowed from kissutil.c...
 * ------------------------------------------------------------------*/
void trim (char *stuff)
{
    char *p;
    p = stuff + strlen(stuff) - 1;
    while (strlen(stuff) > 0 && (*p == '\r' || *p == '\n')) {
      *p = '\0';
      p--;
    }
} // end trim 



// end db.c 
