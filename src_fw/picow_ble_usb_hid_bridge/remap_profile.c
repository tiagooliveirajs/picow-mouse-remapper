#include "remap_profile.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "btstack_tlv.h"
#include "pico/critical_section.h"

#define REMAP_STORE_MAGIC 0x36504d52u /* RMP6 */
#define REMAP_STORE_SCHEMA 1u
#define REMAP_STORE_MAX_RECORDS 8u
#define REMAP_STORE_TAG ((((uint32_t)'R') << 24) | (((uint32_t)'M') << 16) | (((uint32_t)'P') << 8) | '6')

#pragma pack(push, 1)
typedef struct {
    uint8_t valid;
    uint8_t identity_addr_type;
    uint8_t identity_addr[6];
    uint8_t mode;
    uint8_t mappings[REMAP_SOURCE_COUNT];
    uint8_t drag_fix_back;
    uint8_t drag_fix_forward;
    uint8_t reserved[8];
} remap_record_v1_t;

typedef struct {
    uint32_t magic;
    uint16_t schema;
    uint16_t record_size;
    uint32_t generation;
    remap_record_v1_t records[REMAP_STORE_MAX_RECORDS];
    uint32_t checksum;
} remap_store_v1_t;
#pragma pack(pop)

static critical_section_t g_lock;
static bool g_initialized;
static bool g_store_loaded;
static bool g_storage_ok = true;
static remap_store_v1_t g_store;
static remap_profile_snapshot_t g_snapshot;
static bool g_request_pending;
static remap_profile_config_t g_requested_config;
static uint32_t g_requested_id;
static uint32_t g_next_request_id;

static uint32_t fnv1a32(const uint8_t *data,size_t len){uint32_t h=2166136261u;for(size_t i=0;i<len;++i){h^=data[i];h*=16777619u;}return h;}
static uint32_t store_checksum(const remap_store_v1_t *store){return fnv1a32((const uint8_t *)store,offsetof(remap_store_v1_t,checksum));}
static void store_reset(void){memset(&g_store,0,sizeof(g_store));g_store.magic=REMAP_STORE_MAGIC;g_store.schema=REMAP_STORE_SCHEMA;g_store.record_size=sizeof(remap_record_v1_t);g_store.checksum=store_checksum(&g_store);}

static void store_load(void)
{
    if(g_store_loaded)return;g_store_loaded=true;store_reset();const btstack_tlv_t *impl=NULL;void *context=NULL;btstack_tlv_get_instance(&impl,&context);if(impl==NULL){g_storage_ok=false;printf("[PICO-06] remap TLV unavailable; volatile passthrough\n");return;}remap_store_v1_t candidate;const int len=impl->get_tag(context,REMAP_STORE_TAG,(uint8_t *)&candidate,sizeof(candidate));if(len==0){g_storage_ok=true;return;}const bool valid=len==(int)sizeof(candidate)&&candidate.magic==REMAP_STORE_MAGIC&&candidate.schema==REMAP_STORE_SCHEMA&&candidate.record_size==sizeof(remap_record_v1_t)&&candidate.checksum==store_checksum(&candidate);if(!valid){printf("[PICO-06] remap store invalid; fail-safe passthrough\n");g_storage_ok=false;store_reset();return;}memcpy(&g_store,&candidate,sizeof(g_store));g_storage_ok=true;printf("[PICO-06] remap store loaded generation=%lu\n",(unsigned long)g_store.generation);
}

static bool store_commit(void)
{
    const btstack_tlv_t *impl=NULL;void *context=NULL;btstack_tlv_get_instance(&impl,&context);if(impl==NULL){g_storage_ok=false;return false;}g_store.magic=REMAP_STORE_MAGIC;g_store.schema=REMAP_STORE_SCHEMA;g_store.record_size=sizeof(remap_record_v1_t);++g_store.generation;g_store.checksum=store_checksum(&g_store);const int rc=impl->store_tag(context,REMAP_STORE_TAG,(const uint8_t *)&g_store,sizeof(g_store));g_storage_ok=rc==0;if(!g_storage_ok)printf("[PICO-06] remap store commit failed rc=%d\n",rc);return g_storage_ok;
}

static bool identity_equal(uint8_t ta,const uint8_t a[6],uint8_t tb,const uint8_t b[6]){return ta==tb&&memcmp(a,b,6)==0;}
static int find_record(uint8_t type,const uint8_t addr[6]){for(size_t i=0;i<REMAP_STORE_MAX_RECORDS;++i){const remap_record_v1_t *r=&g_store.records[i];if(r->valid&&identity_equal(r->identity_addr_type,r->identity_addr,type,addr))return(int)i;}return-1;}
static int find_free_record(void){for(size_t i=0;i<REMAP_STORE_MAX_RECORDS;++i)if(!g_store.records[i].valid)return(int)i;return-1;}

static bool config_valid(const remap_profile_config_t *config)
{
    if(config==NULL||config->mode>DEVICE_PROFILE_CUSTOM_REMAP||config->drag_fix_back>DEVICE_DRAG_FIX_OFF||config->drag_fix_forward>DEVICE_DRAG_FIX_OFF)return false;bool target_seen[6]={false,false,false,false,false,false};for(size_t i=0;i<REMAP_SOURCE_COUNT;++i){const uint8_t target=config->mappings[i];if(target>REMAP_TARGET_FORWARD)return false;if(config->mode==DEVICE_PROFILE_CUSTOM_REMAP&&target!=0){if(target_seen[target])return false;target_seen[target]=true;}}return true;
}

void remap_profile_make_passthrough(remap_profile_config_t *config){if(config==NULL)return;memset(config,0,sizeof(*config));config->mode=DEVICE_PROFILE_PASSTHROUGH;config->drag_fix_back=DEVICE_DRAG_FIX_AUTO;config->drag_fix_forward=DEVICE_DRAG_FIX_AUTO;}
void remap_profile_make_default(remap_profile_config_t *config){remap_profile_make_passthrough(config);if(config!=NULL)config->mode=DEVICE_PROFILE_DEFAULT_REMAP;}

static remap_profile_config_t config_from_record(const remap_record_v1_t *record){remap_profile_config_t c;remap_profile_make_passthrough(&c);if(record==NULL||!record->valid)return c;c.mode=(device_profile_mode_t)record->mode;memcpy(c.mappings,record->mappings,sizeof(c.mappings));c.drag_fix_back=(device_drag_fix_policy_t)record->drag_fix_back;c.drag_fix_forward=(device_drag_fix_policy_t)record->drag_fix_forward;if(!config_valid(&c))remap_profile_make_passthrough(&c);return c;}
static void record_from_config(remap_record_v1_t *r,uint8_t type,const uint8_t addr[6],const remap_profile_config_t *c){memset(r,0,sizeof(*r));r->valid=1;r->identity_addr_type=type;memcpy(r->identity_addr,addr,6);r->mode=(uint8_t)c->mode;memcpy(r->mappings,c->mappings,sizeof(r->mappings));r->drag_fix_back=(uint8_t)c->drag_fix_back;r->drag_fix_forward=(uint8_t)c->drag_fix_forward;}

static void publish(bool connected,bool restored,uint8_t type,const uint8_t addr[6],const remap_profile_config_t *active,uint32_t apply_id,bool apply_ok)
{
    critical_section_enter_blocking(&g_lock);++g_snapshot.revision;g_snapshot.connected=connected;g_snapshot.storage_ok=g_storage_ok;g_snapshot.restored=restored;g_snapshot.identity_addr_type=type;if(addr!=NULL)memcpy(g_snapshot.identity_addr,addr,6);else memset(g_snapshot.identity_addr,0,6);if(active!=NULL)memcpy(&g_snapshot.active,active,sizeof(*active));else remap_profile_make_passthrough(&g_snapshot.active);if(apply_id!=0){g_snapshot.last_apply_id=apply_id;g_snapshot.last_apply_ok=apply_ok;}critical_section_exit(&g_lock);
}

void remap_profile_init(void){if(g_initialized)return;critical_section_init(&g_lock);memset(&g_snapshot,0,sizeof(g_snapshot));remap_profile_make_passthrough(&g_snapshot.active);g_snapshot.storage_ok=true;g_next_request_id=1;g_initialized=true;}
bool remap_profile_get_snapshot(remap_profile_snapshot_t *snapshot){if(!g_initialized||snapshot==NULL)return false;critical_section_enter_blocking(&g_lock);memcpy(snapshot,&g_snapshot,sizeof(*snapshot));critical_section_exit(&g_lock);return snapshot->revision!=0;}

bool remap_profile_request_apply(const remap_profile_config_t *config,uint32_t *request_id)
{
    if(!g_initialized||!config_valid(config))return false;critical_section_enter_blocking(&g_lock);if(g_request_pending){critical_section_exit(&g_lock);return false;}uint32_t id=g_next_request_id++;if(id==0)id=g_next_request_id++;memcpy(&g_requested_config,config,sizeof(g_requested_config));g_requested_id=id;g_request_pending=true;critical_section_exit(&g_lock);if(request_id!=NULL)*request_id=id;return true;
}

bool remap_profile_delete_saved(uint8_t type,const uint8_t addr[6])
{
    if(!g_initialized||addr==NULL)return false;store_load();const int slot=find_record(type,addr);if(slot<0)return true;remap_store_v1_t previous;memcpy(&previous,&g_store,sizeof(previous));memset(&g_store.records[slot],0,sizeof(g_store.records[slot]));if(!store_commit()){memcpy(&g_store,&previous,sizeof(g_store));return false;}critical_section_enter_blocking(&g_lock);if(g_snapshot.connected&&identity_equal(g_snapshot.identity_addr_type,g_snapshot.identity_addr,type,addr)){g_snapshot.restored=false;remap_profile_make_passthrough(&g_snapshot.active);++g_snapshot.revision;}critical_section_exit(&g_lock);printf("[PICO-07] remap profile deleted addr=%s\n",bd_addr_to_str(addr));return true;
}

static void fail_pending_if_disconnected(void)
{
    remap_profile_config_t requested;uint32_t request_id=0;bool had=false;critical_section_enter_blocking(&g_lock);if(g_request_pending){memcpy(&requested,&g_requested_config,sizeof(requested));request_id=g_requested_id;g_request_pending=false;had=true;}critical_section_exit(&g_lock);(void)requested;if(had){remap_profile_snapshot_t snap;if(remap_profile_get_snapshot(&snap))publish(false,snap.restored,snap.identity_addr_type,snap.identity_addr,&snap.active,request_id,false);}
}

static void process_apply_request(void)
{
    remap_profile_config_t requested;uint32_t request_id=0;bool have=false;critical_section_enter_blocking(&g_lock);if(g_request_pending){memcpy(&requested,&g_requested_config,sizeof(requested));request_id=g_requested_id;g_request_pending=false;have=true;}critical_section_exit(&g_lock);if(!have)return;remap_profile_snapshot_t current;if(!remap_profile_get_snapshot(&current)||!current.connected||!config_valid(&requested)){publish(current.connected,current.restored,current.identity_addr_type,current.identity_addr,&current.active,request_id,false);return;}int slot=find_record(current.identity_addr_type,current.identity_addr);if(slot<0)slot=find_free_record();if(slot<0){printf("[PICO-06] remap table full\n");publish(true,current.restored,current.identity_addr_type,current.identity_addr,&current.active,request_id,false);return;}remap_store_v1_t previous;memcpy(&previous,&g_store,sizeof(previous));record_from_config(&g_store.records[slot],current.identity_addr_type,current.identity_addr,&requested);if(!store_commit()){memcpy(&g_store,&previous,sizeof(g_store));publish(true,current.restored,current.identity_addr_type,current.identity_addr,&current.active,request_id,false);return;}
    (void)device_profile_update_profile_summary(requested.mode,requested.drag_fix_back,requested.drag_fix_forward);
    printf("[PICO-06] profile applied id=%lu mode=%u\n",(unsigned long)request_id,(unsigned)requested.mode);publish(true,true,current.identity_addr_type,current.identity_addr,&requested,request_id,true);
}

void remap_profile_core1_task(void)
{
    if(!g_initialized)return;store_load();device_profile_snapshot_t device;const bool have_device=device_profile_get_snapshot(&device);if(!have_device||!device.connected){remap_profile_snapshot_t current;const bool have_current=remap_profile_get_snapshot(&current);if(have_current&&current.connected)publish(false,current.restored,current.identity_addr_type,current.identity_addr,&current.active,0,false);fail_pending_if_disconnected();return;}remap_profile_snapshot_t current;const bool have_current=remap_profile_get_snapshot(&current);const bool identity_changed=!have_current||!current.connected||!identity_equal(current.identity_addr_type,current.identity_addr,device.identity_addr_type,device.identity_addr);
    if(identity_changed){const int slot=find_record(device.identity_addr_type,device.identity_addr);remap_profile_config_t active;const bool restored=slot>=0;if(restored)active=config_from_record(&g_store.records[slot]);else remap_profile_make_passthrough(&active);if((device.capabilities&DEVICE_CAP_MOUSE)!=0)(void)device_profile_update_profile_summary(active.mode,active.drag_fix_back,active.drag_fix_forward);printf("[PICO-06] remap profile %s mode=%u\n",restored?"RESTORED":"NEW PASSTHROUGH",(unsigned)active.mode);publish(true,restored,device.identity_addr_type,device.identity_addr,&active,0,false);}process_apply_request();
}

const char *remap_profile_mode_name(device_profile_mode_t mode){switch(mode){case DEVICE_PROFILE_DEFAULT_REMAP:return "DEFAULT";case DEVICE_PROFILE_CUSTOM_REMAP:return "CUSTOM";default:return "PASSTHRU";}}
const char *remap_source_name(remap_source_t source){switch(source){case REMAP_SOURCE_LEFT:return "LEFT";case REMAP_SOURCE_RIGHT:return "RIGHT";case REMAP_SOURCE_MIDDLE:return "MIDDLE";case REMAP_SOURCE_BACK:return "BACK";case REMAP_SOURCE_FORWARD:return "FORWARD";default:return "?";}}
const char *remap_target_name(remap_target_t target){switch(target){case REMAP_TARGET_LEFT:return "LEFT";case REMAP_TARGET_RIGHT:return "RIGHT";case REMAP_TARGET_MIDDLE:return "MIDDLE";case REMAP_TARGET_BACK:return "BACK";case REMAP_TARGET_FORWARD:return "FORWARD";default:return "PASS";}}
