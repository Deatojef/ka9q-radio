// maximum packet size in bytes (basically the maximum size of an AX.25 UI-Frame)
#define MAX_PACKET_SIZE 384

// maximum size of a callsign in characters.  Ham radio only "technically" allows for a 9-character callsign-ssid (5 for the callsign, 1 for the '-' character, 2 for the SSID, and 1 for the ending \0 character).  However, with the advent of other callsigns being used in the aprs cloud (ex. LoRA, internet only-based callsigns, etc), nudging this 'maximum' size up to accomodate other, non-RF based callsigns.
#define MAX_CALLSIGN_SIZE 16

// ------------------------------------------------------
//  payload, node, queue
// ------------------------------------------------------

// the payload structure (that is inserted into the double linked list queue...by way of the 'node_t' structure)
typedef struct payload_tag {
    char *packet;
    bool heard_direct;
    bool is_satellite;
    int frequency;
    char heard_from[MAX_CALLSIGN_SIZE];
    char callsign[MAX_CALLSIGN_SIZE];
} payload_t;

// node structure, these are the elements of our double linked list
typedef struct node_tag {
    struct node_tag *next;
    struct node_tag *prev;
    struct payload_t *payload;
} node_t;

// the queue...has the start and end of our linked list of nodes
typedef struct queue_tag {
    struct node_t *end;
    struct node_t *start;
    int n;  // number of items in this queue
} queue_t;


// functions for adding and removing nodes from a queue
int enqueue(struct queue_t *Q, struct node_t *new_entry, mutex_t *queue_mutex);
int dequeue(struct queue_t *Q, struct node_t *node, mutex_t *queue_mutex);

// ------------------------------------------------------
// ------------------------------------------------------



// ------------------------------------------------------
//  logging
// ------------------------------------------------------

// debug levels
typedef enum {
    DEBUG,
    INFO,
    WARN,
    ERROR,
    FATAL
} severity_t;

typedef enum {
    RTP_INPUT,      // real time protocol input operations
    GPSD,           // connections to the GPSD daemon
    IGATE,          // igating operations
    BEACONING,      // beaconing to the APRS-IS cloud
    APRS_PARSING,   // parsing APRS 
    DATABASE,       // database operations
    PACKET_QUEUE    // packet queue operations
} domains_t;

typedef struct module_logging_tag {


extern struct logging_domain_t logging_domains[];


// Wrapper for printf that allows for filtering & formating log output
int log_printf(debug_domain, severity, *fmt, ...) {

    switch(debug_domain) {
        case 
    if (severity > debug_level

