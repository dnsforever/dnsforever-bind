#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include <iostream>
#include <memory>
#include <list>
#include <map>

using namespace std;

#include <pthread.h>

#include <jsoncpp/json/json.h>

#include <curl/curl.h>
#include <curl/easy.h>

extern "C" {
#include "../modules/include/dlz_minimal.h"
}

class DnsforeverRecord {
public:
    DnsforeverRecord(string subname, string type, string rdata, int ttl);
    virtual ~DnsforeverRecord();

    inline string get_subname() {return subname;};
    inline string get_type() {return type;};
    inline string get_rdata() {return rdata;};
    inline int get_ttl() {return ttl;};

protected:
    string subname;
    string type;
    string rdata;
    int ttl;
};

class DnsforeverZone {
public:
    DnsforeverZone();
    virtual ~DnsforeverZone();

    bool add_record(string subname, string type, string rdata, int ttl);
    map<string, list<shared_ptr<DnsforeverRecord> > > records;
};

class Dnsforever {
public:
    Dnsforever(string name, string server, unsigned int update_interval);
    virtual ~Dnsforever();

    inline string get_name() {return name;};

    shared_ptr<DnsforeverZone> lookup_zone(string domain);
    void remove_zone(string zone);

    void add_helper(const char *helper_name, void *ptr);

    string name;
    string server;

    map<string, shared_ptr<DnsforeverZone> > zones;

    unsigned int last_update;
    unsigned int update_interval;
    pthread_t thread;
    bool start_update();
    void end_update();
    void update();

    log_t *log;
    dns_sdlz_putrr_t *putrr;
    dns_sdlz_putnamedrr_t *putnamedrr;
    dns_dlz_writeablezone_t *writeable_zone;
};

DnsforeverRecord::DnsforeverRecord(string name, string type, string rdata, int ttl) {
    this->subname = subname;
    this->type = type;
    this->rdata = rdata;
    this->ttl = ttl;
}

DnsforeverRecord::~DnsforeverRecord() {
}

DnsforeverZone::DnsforeverZone() {
}

DnsforeverZone::~DnsforeverZone() {
}

Dnsforever::Dnsforever(string name, string server, unsigned int update_interval) {
    this->name = name;
    this->server = server;
    this->update_interval = update_interval;
    this->last_update = 0;

    this->log = NULL;
    this->putrr = NULL;
    this->putnamedrr = NULL;
    this->writeable_zone = NULL;
}

Dnsforever::~Dnsforever() {
    this->update_interval = 0;
    pthread_join(this->thread, NULL);
}

bool
single_valued(string type) {
    string single_types[]= {"soa", "cname"};

    for (auto single_type: single_types)
        if (single_type == type)
            return true;
    return false;
}

bool
DnsforeverZone::add_record(string subname, string type, string rdata, int ttl) {
    auto record_list = records[subname];

    if (single_valued(type))
        for (auto record: record_list)
            if (record->get_type() == type)
                return false;

    for (auto record: record_list)
        if (record->get_type() == type && record->get_rdata() == rdata)
            return false;

    record_list.push_back(make_shared<DnsforeverRecord>(subname, type, rdata, ttl));
    records[subname] = record_list;
    return true;
}

shared_ptr<DnsforeverZone>
Dnsforever::lookup_zone(string domain) {
    shared_ptr<DnsforeverZone> zone;
    size_t dot_pos;
    while ((dot_pos = domain.find('.')) != -1) {
        zone = zones[domain];
        if (zone)
            return zone;

        domain = domain.substr(dot_pos);
    }
    return zone;
}

void
Dnsforever::remove_zone(string name) {
    zones.erase(name);
}

static void *update_thread(void* arg) {
    Dnsforever* dnsforever = (Dnsforever*)arg;

    do {
        sleep(dnsforever->update_interval);
        dnsforever->log(ISC_LOG_INFO,
                        "dnsforever: update %d", dnsforever->update_interval);
        dnsforever->update();
    } while (dnsforever->thread && dnsforever->update_interval);

    pthread_exit(NULL);
    return NULL;
}

bool Dnsforever::start_update()
{
    int rc = pthread_create(&thread, NULL, update_thread, (void*)this);
    if (rc)
        return false;

    return true;
}

static size_t write_callback(char *contents, size_t size, size_t nmemb, void *userp)
{
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

void Dnsforever::update() {
    CURL *curl;
    CURLcode res;
    std::string readBuffer;

    curl = curl_easy_init();
    if(!curl) {
        log(ISC_LOG_INFO, "dnsforever: curl init failed");
        return;
    }

    curl_easy_setopt(curl, CURLOPT_URL, server.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
    res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if(res != CURLE_OK) {
        log(ISC_LOG_INFO, "dnsforever: curl failed(%d) %s", res, server.c_str());
        return;
    }

    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(readBuffer, root)) {
        log(ISC_LOG_INFO, "dnsforever: parse failed(0)");
        return;
    }

    if (!root.isObject() || root.size() <= 0) {
        log(ISC_LOG_INFO, "dnsforever: parse failed(1)");
        return;
    }

    for(Json::ValueIterator itr = root.begin() ; itr != root.end() ; itr++) {
        auto zone = make_shared<DnsforeverZone>();

        std::string zone_name = itr.key().asString();
        Json::Value zone_info = root[zone_name];

        Json::Value records = zone_info["records"];
        if (records.isArray()) {
            for(auto record: records) {
                std::string s = record.asString();
                std::string delimiter = " ";

                size_t name_end = s.find(delimiter);
                std::string name = s.substr(0, name_end);

                size_t type_end = s.find(delimiter, name_end + 1);
                std::string type = s.substr(name_end + 1, type_end - (name_end + 1));

                std::string rdata = s.substr(type_end + 1);

                zone->add_record(name, type, rdata, 300);
            }
        }

        unsigned long last_update = zone_info["last_update"].asUInt();
        if(this->last_update < last_update)
            this->last_update = last_update;

        //if(zones[zone_name])
        //    log(ISC_LOG_INFO, "dnsforever: update zone [%s]", zone_name.c_str());
        //else
        //    log(ISC_LOG_INFO, "dnsforever: add zone [%s]", zone_name.c_str());
        zones[zone_name] = zone; // WLOCK
    }
}

/*
 * Return the version of the API
 */
int
dlz_version(unsigned int *flags) {
    UNUSED(flags);
    return (DLZ_DLOPEN_VERSION);
}

/*
 * Remember a helper function from the bind9 dlz_dlopen driver
 */
void Dnsforever::add_helper(const char *helper_name, void *ptr)
{
    if (strcmp(helper_name, "log") == 0)
        this->log = (log_t *)ptr;
    if (strcmp(helper_name, "putrr") == 0)
        this->putrr = (dns_sdlz_putrr_t *)ptr;
    if (strcmp(helper_name, "putnamedrr") == 0)
        this->putnamedrr = (dns_sdlz_putnamedrr_t *)ptr;
    if (strcmp(helper_name, "writeable_zone") == 0)
        this->writeable_zone = (dns_dlz_writeablezone_t *)ptr;
}

/*
 * Called to initialize the driver
 */
isc_result_t
dlz_create(const char *dlzname, unsigned int argc, char *argv[],
       void **dbdata, ...)
{
    if (argc != 3)
        return ISC_R_FAILURE;

    string server(argv[1]);
    unsigned int update = atoi(argv[2]);

    Dnsforever *dnsforever = new Dnsforever(dlzname, server, update);
    *dbdata = (void *)dnsforever;

    const char *helper_name;
    va_list ap;
    va_start(ap, dbdata);
    while ((helper_name = va_arg(ap, const char *)) != NULL) {
        dnsforever->add_helper(helper_name, va_arg(ap, void *));
    }
    va_end(ap);

    if (!dnsforever->start_update())
        return (ISC_R_FAILURE);

    dnsforever->log(ISC_LOG_INFO,
                    "dnsforever: started for zone %s %s",
                    dnsforever->get_name().c_str(),
                    dnsforever->server.c_str());

    return (ISC_R_SUCCESS);
}

/*
 * Shut down the backend
 */
void
dlz_destroy(void *dbdata) {
    Dnsforever *dnsforever = (Dnsforever*)dbdata;

    dnsforever->log(ISC_LOG_INFO,
                    "dnsforever: shutting down zone %s %s",
                    dnsforever->get_name().c_str(),
                    dnsforever->server.c_str());

    delete dnsforever;
}

/*
 * See if we handle a given zone
 */
isc_result_t
dlz_findzonedb(void *dbdata, const char *name,
       dns_clientinfomethods_t *methods,
       dns_clientinfo_t *clientinfo)
{
    Dnsforever *dnsforever = (Dnsforever*)dbdata;

    dnsforever->log(ISC_LOG_INFO, "dnsforever: findzone %s", name);
    if (dnsforever->zones[name])
        return (ISC_R_SUCCESS);

    return (ISC_R_NOTFOUND);
}

/*
 * Look up one record in the sample database.
 */
isc_result_t
dlz_lookup(const char *zone, const char *name, void *dbdata,
       dns_sdlzlookup_t *lookup, dns_clientinfomethods_t *methods,
       dns_clientinfo_t *clientinfo)
{
    Dnsforever *dnsforever = (Dnsforever*)dbdata;
    if (dnsforever->putrr == NULL)
        return (ISC_R_NOTIMPLEMENTED);

    dnsforever->log(ISC_LOG_INFO, "dnsforever: lookup %s->%s", zone, name);

    shared_ptr<DnsforeverZone> zone_info = dnsforever->zones[zone];
    if (!zone_info)
        return (ISC_R_FAILURE);

    isc_result_t result;
    isc_boolean_t found = ISC_FALSE;
    for (auto record: zone_info->records[name]) {
        found = ISC_TRUE;
        result = dnsforever->putrr(lookup, record->get_type().c_str(),
                                   record->get_ttl(),
                                   record->get_rdata().c_str());
        dnsforever->log(ISC_LOG_INFO, "dnsforever: >> [%s] %d %s",
                        record->get_type().c_str(),
                        record->get_ttl(),
                        record->get_rdata().c_str());

        if (result != ISC_R_SUCCESS) {
            dnsforever->log(ISC_LOG_INFO, "dnsforever: lookup failed(%d)", result);
            return (result);
        }
    }

    if (!found)
        return (ISC_R_NOTFOUND);

    return (ISC_R_SUCCESS);
}
