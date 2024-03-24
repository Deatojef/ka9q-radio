// db.h 
#include <stdbool.h>

// This is for postgresql
#ifdef __APPLE__
#include </opt/local/include/postgresql96/libpq-fe.h>
#else
#include <postgresql/libpq-fe.h>
#endif



// db_init opens a DB connection to postgresql 
bool db_init (char *host, char *dbname, char *user, char *table, char *passfile);

//  db_write will write a packet to the backend database
bool db_write(long long timestamp, int gps_mode, double gps_lat, double gps_lon, int ssrc, int lna_gain, int mixer_gain, int if_gain, double input_power_dbfs, double frontend_gain_db, double baseband_power_db, double snr_db);

// db_term closes all database connections
void db_term(void);

// make_safe_text creates a string of the raw packet text that is safe for printing (or...for adding to a DB field)
char * make_ascii_only(char *pstr, int len, char *safe_str, int dest_size);

// to trim off trailing \r and \n characters
void trim(char *stuff);
