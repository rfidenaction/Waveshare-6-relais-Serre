// =============================================================================
// main.cpp - Point d'entree principal
// LilyGo T-SIM7080G-S3 - Routeur WiFi/GSM + Service SMS
//
// Sequence de demarrage :
//   1. NVS + netif + event loop
//   2. PMU AXP2101 (alimentation modem via I2C)
//   3. GPIO modem (PWRKEY, DTR, RI)
//   4. esp_modem : essai AT, PWRKEY seulement si pas de reponse (cf. exemple LilyGo)
//   5. PPPoS → attente IP
//   6. WiFi SoftAP
//   7. NAPT (DNS + NAT L3)
//   8. Queue + Mutex + Table SMS (AVANT le serveur HTTP)
//   9. Serveur HTTP — DÉSACTIVÉ (v9.0 — remplacé par UDP)
//  10. Tache SMS (consomme la queue, envoi reel via API esp_modem)
//  11. Tache reconnexion PPP (dort tant que PPP est OK, se reveille sur perte)
//  12. Tache UDP Bridge (communication Waveshare ↔ LilyGo)
//  13. SMS de boot (signe de vie LilyGo avec pourcentage batterie)
//
// Communication Waveshare ↔ LilyGo (v9.0 — UDP) :
//   - UDP unicast bidirectionnel (LilyGo:5000 ↔ Waveshare:5001)
//   - Paquets : HB (heartbeat), SMS|number|text, STATE|0/1, ACK
//   - Remplace le serveur HTTP (heartbeat, POST /sms, GET /sms)
//   - Le serveur HTTP est conservé en commentaire (#if 0)
//
// Service SMS (etape 2c — envoi via API esp_modem) :
//   - Ordres SMS reçus par UDP depuis la Waveshare
//   - Tache SMS : consomme la queue, verifie TTL, rate limit, bascule PPP/COMMAND,
//     envoi par esp_modem_send_sms(), reprise DATA MODE (toujours)
//   - ACK envoyé par UDP quand le modem confirme l'envoi
//   - Rate limit sur toute tentative (reussie ou pas)
//   - Table de statuts circulaire en RAM, protegee par mutex
//
// Reconnexion PPP :
//   - Declenchee par IP_EVENT_PPP_LOST_IP
//   - Backoff progressif plafonne a 120s, tentatives infinies
//   - Mutex modem_mode_mutex : empeche les conflits avec la tache SMS
//   - Watchdog : reboot si aucun heartbeat pendant 2h ET PPP down
//   - enable_napt() appele apres reconnexion reussie (reconfiguration DNS/NAT)
//
// --- Historique ---
// v8.3-diag : ajout diagnostics reseau, identification code mort
// v8.4-fix-route : correction restauration route par defaut apres SMS
// v8.5-test-include : nettoyage includes
// v9.0-udp-bridge : remplacement HTTP par UDP pour communication Waveshare
//   - Ajout tache udp_bridge_task (reception HB/SMS, envoi STATE/ACK)
//   - Ajout can_accept_sms() (disponibilite LilyGo pour SMS)
//   - Ajout sms_processing flag dans sms_task
//   - Ajout from_udp dans sms_entry_t (ACK conditionnel)
//   - Rate limit sur toute tentative (pas seulement succes)
//   - SMS de boot avec pourcentage batterie (interpolation 3.0-4.2V)
//   - Serveur HTTP mis en commentaire (#if 0)
//   - Queue SMS reduite de 10 a 2
// =============================================================================

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/uart.h"

// PMU AXP2101
#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

// esp_modem (PPPoS + module SIM7070)
#include "esp_modem_api.h"

// NAPT (NAT L3) — API lwIP directe (esp_netif_napt_enable n'existe pas en ESP-IDF 5.0.2)
#include "lwip/lwip_napt.h"

// Route par defaut — pour forcer PPP comme gateway internet
#include "lwip/netif.h"
// [v8.4] CONSERVE pour investigation phase 2 — a commenter en v8.5 pour
// verifier s'il a un effet de bord sur le fonctionnement MQTT.
// #include "esp_netif_net_stack.h"

// Serveur HTTP + JSON (inclus dans ESP-IDF)
#include "esp_http_server.h"
#include "cJSON.h"

// UDP sockets (communication Waveshare ↔ LilyGo)
#include "lwip/sockets.h"

// Ping ICMP (test de connectivite Internet reelle)
#include "ping/ping_sock.h"


// =============================================================================
// Configuration materielle (voir document architecture §3)
// =============================================================================

// --- WiFi SoftAP ---
#define WIFI_AP_SSID        "Pont_Wifi-GSM_de_la_serre"
#define WIFI_AP_PASSWORD    "1234567890"  //Non utiiisé maintenant
#define WIFI_AP_CHANNEL     1
#define WIFI_AP_MAX_CONN    1       // Waveshare uniquement
#define WIFI_AP_IP          "192.168.4.1"
#define WIFI_AP_NETMASK     "255.255.255.0"

// --- PMU AXP2101 (I2C) ---
#define PMU_I2C_SDA         15
#define PMU_I2C_SCL         7

// --- Modem SIM7080G (UART + GPIO) ---
#define MODEM_UART_TX       GPIO_NUM_5      // ESP → SIM
#define MODEM_UART_RX       GPIO_NUM_4      // SIM → ESP
#define MODEM_UART_BAUD     115200
#define MODEM_PWR_PIN       GPIO_NUM_41     // PWRKEY
#define MODEM_DTR_PIN       GPIO_NUM_42     // DTR
#define MODEM_RI_PIN        GPIO_NUM_3      // RI (Ring Indicator)

// --- APN ---
#define MODEM_APN           "domotec82.fr"

// --- SMS (voir document architecture §3 et §7) ---
#define SMS_CENTER          "+32495005580"  // Centre SMS operateur
#define SMS_TEXT_MAX_LEN    160             // Limite GSM ASCII standard
#define SMS_TTL_DEFAULT     300             // TTL par defaut en secondes (5 min)
#define SMS_QUEUE_SIZE      2               // Taille de la queue FreeRTOS (réduite, 2 SMS max)
#define SMS_TABLE_SIZE      20              // Taille de la table de statuts (circulaire)
#define SMS_RATE_LIMIT_MS   300000          // Rate limit global : 5 minutes (300s) entre deux envois reussis
#define SMS_PPP_CHECK_MS    5000            // Intervalle de verification PPP quand inactif (5s)

// --- SMS : pas de defines AT manuels ---
// L'envoi SMS utilise l'API esp_modem (esp_modem_send_sms) qui gere
// internement la sequence AT+CMGF/AT+CMGS/Ctrl+Z et les timeouts.

// --- UDP Bridge (communication Waveshare ↔ LilyGo) ---
#define UDP_LOCAL_PORT       5000            // Port écoute LilyGo
#define UDP_REMOTE_PORT      5001            // Port écoute Waveshare
#define WAVESHARE_IP         "192.168.4.10"  // IP fixe Waveshare
#define STATE_INTERVAL_MS    30000           // Envoi STATE toutes les 30s
#define UDP_TASK_STACK       4096
#define UDP_TASK_PRIORITY    3               // Sous sms_task(5) et ppp_reconnect(4)

// Whitelist des numeros autorises (voir architecture §7)
// Pour l'instant un seul destinataire. Extensible en tableau si besoin.
static const char *sms_whitelist[] = {
    "+33672967933"
};
#define SMS_WHITELIST_SIZE  (sizeof(sms_whitelist) / sizeof(sms_whitelist[0]))

// --- Timings ---
#define MODEM_STABILIZE_MS  5000    // Delai apres enableDC3 / avant premier AT
#define AT_BEFORE_PWRKEY    6       // Nombre d'essais AT avant de pulser PWRKEY (cf. LilyGo)
#define AT_RETRY_MAX        20      // Nombre max total de tentatives AT
#define PPP_TIMEOUT_MS      60000   // Timeout connexion PPP

// --- Reconnexion PPP ---
#define PPP_RECONNECT_BIT       BIT1    // Signal pour la tache de reconnexion
#define PPP_RECONNECT_MAX       5       // Nombre de paliers dans le backoff
// Backoff progressif en ms : 15s → 30s → 60s → 120s → 120s (plafond a 120s ensuite)
static const int ppp_reconnect_backoff_ms[PPP_RECONNECT_MAX] = {
    15000, 30000, 60000, 120000, 120000
};

// =============================================================================
// Parametres configurables — comportement modifiable sans toucher au code
// =============================================================================

// --- SMS de boot (signe de vie LilyGo au demarrage) ---
#define BOOT_SMS_ENABLED                false    // true = envoie un SMS au demarrage
                                                // false = pas de SMS de boot

// --- Surveillance Waveshare : alerte SMS si heartbeat absent ---
// La Waveshare envoie un heartbeat regulier. Si elle cesse de repondre
// pendant la duree ci-dessous, un SMS d'alerte est envoye (une seule fois).
// Le SMS est renvoye uniquement si un heartbeat revient puis disparait a nouveau.
#define ALERTE_ABSENCE_HEARTBEAT_SMS_ENABLED    true    // true = SMS si Waveshare muette
                                                        // false = pas d'alerte SMS
#define ALERTE_ABSENCE_HEARTBEAT_DELAI_MS       3600000 // Delai sans heartbeat avant SMS (1h)

// --- Surveillance PPP : relance forcee + reboot si panne durable ---
// Mecanisme independant de ppp_reconnect_task (qui peut rester endormie si
// l'evenement IP_EVENT_PPP_LOST_IP n'a pas ete emis). udp_bridge_task surveille
// l'etat PPP en continu et agit sur deux seuils :
//   - 5 min de PPP down : reveil force de ppp_reconnect_task (positionne
//     PPP_RECONNECT_BIT sans tester son etat interne, par robustesse)
//   - 2h de PPP down : reboot complet de la LilyGo
// Le reveil force est repete tant que PPP est down (toutes les 5 min).
// Le reboot ne tient PLUS compte du heartbeat Waveshare : la Waveshare est
// stable, on peut avoir besoin de rebooter la LilyGo seule.
#define WATCHDOG_REBOOT_ENABLED                 true    // true = reboot actif
                                                        // false = pas de reboot auto (debug)
#define PPP_DOWN_RELAUNCH_MS                    300000UL    // 5 min : relance forcee PPP
#define PPP_DOWN_REBOOT_MS                      7200000UL   // 2h : reboot si PPP toujours down

// --- Test de connectivite Internet reelle (ping ICMP) ---
// PPP_CONNECTED_BIT peut etre UP alors que la connectivite Internet est cassee
// (cas observe : glitch GSM ou NAPT desynchronise apres bref hoquet PPP).
// Pour detecter ce "PPP fantome", on teste reellement Internet par ping ICMP.
//
// Logique :
//   - Toutes les 40 min : ping vers 1.1.1.1 (Cloudflare, anycast tres fiable)
//   - Si echec : on re-essaie 8.8.8.8 (Google) puis 9.9.9.9 (Quad9). Il faut
//     que les 3 DNS publics majeurs ne repondent pas pour conclure que c'est
//     nous qui avons un probleme (et pas une panne de Cloudflare).
//   - Si les 3 echouent : on force une renegociation PPP, puis on retente.
//   - Si 3 cycles consecutifs echouent (~2h) : reboot complet.
//
// 40 min est un bon compromis pour cette serre :
//   - Bien plus long que les transitoires GSM normaux
//   - Bien plus court que la cadence des donnees (1h)
//   - Consommation data negligeable (~1 ping = 64 octets)
#define PING_CHECK_INTERVAL_MS                  2400000UL   // 40 min entre 2 cycles
#define PING_TIMEOUT_MS                         5000        // 5s par tentative
#define PING_MAX_FAILURES                       3           // Reboot apres 3 cycles KO
#define PING_TARGET_PRIMARY                     "1.1.1.1"   // Cloudflare
#define PING_TARGET_FALLBACK_1                  "8.8.8.8"   // Google
#define PING_TARGET_FALLBACK_2                  "9.9.9.9"   // Quad9

// Delai de grace au boot : aucun ping pendant les 15 premieres minutes apres
// demarrage de udp_bridge_task. Garantit qu'aucune action de surveillance
// active n'interfere avec :
//   - Etape 13 du boot LilyGo (SMS de boot)
//   - Demarrage differe BridgeManager cote Waveshare (4 min)
//   - SMS de bienvenue Waveshare et son cooldown 5 min
//   - Toute autre activite d'initialisation
// Le mecanisme PPP_CONNECTED_BIT (5 min relance, 2h reboot) reste actif et
// inchange : il ne touche au modem que sur PPP REELLEMENT down.
#define PING_BOOT_GRACE_MS                      900000UL    // 15 min

// --- Surveillance batterie : alerte SMS si coupure secteur ---
// Si la batterie descend sous le seuil bas (secteur probablement coupe),
// un SMS d'alerte est envoye. Quand elle remonte au-dessus du seuil haut
// (secteur retabli), un SMS de retablissement est envoye.
// Le SMS de retablissement est reessaye automatiquement toutes les 30s
// si le rate limit le bloque temporairement.
#define ALERTE_BATTERIE_ENABLED                 true    // true = surveillance batterie active
                                                        // false = pas d'alerte batterie
#define ALERTE_BATTERIE_SEUIL_BAS_POURCENT      90      // SMS si batterie descend sous ce seuil
#define ALERTE_BATTERIE_SEUIL_HAUT_POURCENT     98      // SMS quand batterie remonte au-dessus

// Tag logs
static const char *TAG = "ROUTEUR";

// =============================================================================
// Variables globales
// =============================================================================

// Event group pour synchroniser la connexion PPP
static EventGroupHandle_t modem_event_group;
#define PPP_CONNECTED_BIT   BIT0

// Interfaces reseau (conservees pour le NAPT)
static esp_netif_t *ap_netif  = NULL;
static esp_netif_t *ppp_netif = NULL;

// PMU
static XPowersAXP2101 pmu;

// DCE modem — global car utilise par sms_task ET ppp_reconnect_task
static esp_modem_dce_t *dce = NULL;

// Mutex pour proteger les bascules COMMAND/DATA du modem.
// Pris par : sms_task (etapes 4-5-6) et ppp_reconnect_task.
// Garantit qu'une seule sequence de bascule est en cours a la fois.
static SemaphoreHandle_t modem_mode_mutex = NULL;

// Watchdog : timestamp de la derniere activite confirmee (ms depuis boot).
// Mis a jour par : GET /heartbeat (Waveshare confirme MQTT OK).
// Initialise au boot pour donner 2h de grace au demarrage.
// Protege par spinlock (int64_t non atomique sur ESP32 32-bit).
static int64_t last_activity_ms = 0;
static portMUX_TYPE activity_spinlock = portMUX_INITIALIZER_UNLOCKED;

// =============================================================================
// SMS — Structures de donnees (etape 2c)
// =============================================================================

// Statuts possibles d'un SMS (voir architecture §5.1.B)
typedef enum {
    SMS_STATUS_PENDING,     // En file d'attente
    SMS_STATUS_SENDING,     // Bascule PPP/AT en cours
    SMS_STATUS_SENT,        // Envoye avec succes
    SMS_STATUS_FAILED,      // Echec d'envoi
    SMS_STATUS_EXPIRED      // TTL depasse avant envoi
} sms_status_t;

// Convertit un statut en chaine pour les logs et reponses JSON
static const char *sms_status_to_str(sms_status_t status)
{
    switch (status) {
        case SMS_STATUS_PENDING:  return "PENDING";
        case SMS_STATUS_SENDING:  return "SENDING";
        case SMS_STATUS_SENT:     return "SENT";
        case SMS_STATUS_FAILED:   return "FAILED";
        case SMS_STATUS_EXPIRED:  return "EXPIRED";
        default:                  return "UNKNOWN";
    }
}

// Entree dans la table de statuts
typedef struct {
    bool used;                  // Slot occupe ou libre
    char request_id[33];        // Identifiant unique (max 32 chars)
    char to[21];                // Numero destinataire (E.164 : +15 chiffres max)
    char text[161];             // Texte du SMS (max 160 chars)
    char source[33];            // Source emetteur (optionnel)
    int ttl_s;                  // Duree de validite en secondes
    int64_t created_ms;         // Timestamp creation (esp_timer, ms depuis boot)
    sms_status_t status;        // Statut courant
    char error_code[33];        // Code erreur AT (si FAILED)
    char error_detail[65];      // Detail erreur (si FAILED)
    bool from_udp;              // true si ce SMS vient de l'UDP (ACK attendu)
} sms_entry_t;

// Table de statuts circulaire — protegee par mutex
static sms_entry_t sms_table[SMS_TABLE_SIZE];
static int sms_table_next = 0;  // Prochain index d'ecriture (circulaire)

// Queue FreeRTOS : contient les index dans sms_table
static QueueHandle_t sms_queue = NULL;

// Mutex pour proteger la table (acces concurrent : HTTP handlers + tache SMS)
static SemaphoreHandle_t sms_mutex = NULL;

// Rate limit : timestamp du dernier SMS envoye (reussi ou pas) (ms depuis boot)
// Protege par sms_mutex. Valeur 0 = aucun SMS encore envoye.
static int64_t last_sms_sent_ms = 0;

// =============================================================================
// UDP Bridge — Variables globales
// =============================================================================

// Socket UDP (cree par udp_bridge_task)
static int udp_sock = -1;

// Adresse cible Waveshare pour STATE et ACK
static struct sockaddr_in waveshare_addr;

// Flag : sms_task est en train de traiter un SMS
// Utilise par can_accept_sms() pour indiquer que la LilyGo est occupee.
static volatile bool sms_processing = false;

// Flag : un SMS d'alerte "Waveshare muette" a deja ete envoye.
// Remis a false quand un heartbeat revient (evite le spam SMS).
static volatile bool alerte_heartbeat_sms_envoye = false;

// Flags alerte batterie (surveillance coupure secteur).
// alerte_batterie_basse_envoyee : true apres envoi du SMS "batterie basse"
// alerte_batterie_retablie_a_envoyer : true quand la batterie remonte, SMS pas encore parti
static volatile bool alerte_batterie_basse_envoyee = false;
static volatile bool alerte_batterie_retablie_a_envoyer = false;

// =============================================================================
// 1. PMU AXP2101 — Alimentation modem
// =============================================================================

static esp_err_t pmu_init(void)
{
    ESP_LOGI(TAG, "[PMU] Initialisation AXP2101 (I2C SDA=%d SCL=%d)...",
             PMU_I2C_SDA, PMU_I2C_SCL);

    bool result = pmu.begin((i2c_port_t)0, AXP2101_SLAVE_ADDRESS,
                            PMU_I2C_SDA, PMU_I2C_SCL);
    if (!result) {
        ESP_LOGE(TAG, "[PMU] ECHEC : AXP2101 non detecte sur I2C !");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "[PMU] AXP2101 detecte");

    pmu.disableDC3();
    ESP_LOGI(TAG, "[PMU] DC3 desactive (power cycle)");
    vTaskDelay(pdMS_TO_TICKS(500));

    pmu.setDC3Voltage(3000);
    pmu.enableDC3();
    ESP_LOGI(TAG, "[PMU] DC3 = 3000mV (modem) — ACTIVE (apres power cycle)");

    pmu.setBLDO2Voltage(3300);
    pmu.enableBLDO2();
    ESP_LOGI(TAG, "[PMU] BLDO2 = 3300mV (antenne) — ACTIVE");

    pmu.disableTSPinMeasure();
    ESP_LOGI(TAG, "[PMU] TS Pin measure desactive");

    ESP_LOGI(TAG, "[PMU] Initialisation terminee");
    return ESP_OK;
}

// =============================================================================
// 2. GPIO modem + PWRKEY
// =============================================================================

static void modem_gpio_init(void)
{
    ESP_LOGI(TAG, "[MODEM] Configuration GPIO...");

    gpio_config_t out_conf = {};
    out_conf.pin_bit_mask = (1ULL << MODEM_PWR_PIN) | (1ULL << MODEM_DTR_PIN);
    out_conf.mode = GPIO_MODE_OUTPUT;
    out_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    out_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    out_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&out_conf);

    gpio_config_t in_conf = {};
    in_conf.pin_bit_mask = (1ULL << MODEM_RI_PIN);
    in_conf.mode = GPIO_MODE_INPUT;
    in_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    in_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    in_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&in_conf);

    gpio_set_level(MODEM_DTR_PIN, 0);
    ESP_LOGI(TAG, "[MODEM] DTR = LOW (mode normal)");
}

static void modem_pulse_pwrkey(void)
{
    ESP_LOGI(TAG, "[MODEM] Pulse PWRKEY : LOW → 100ms → HIGH → 1000ms → LOW");
    gpio_set_level(MODEM_PWR_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(MODEM_PWR_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(1000));
    gpio_set_level(MODEM_PWR_PIN, 0);

    ESP_LOGI(TAG, "[MODEM] Attente stabilisation %d ms...", MODEM_STABILIZE_MS);
    vTaskDelay(pdMS_TO_TICKS(MODEM_STABILIZE_MS));
}

// =============================================================================
// 3. Gestionnaire evenements IP (PPPoS)
// =============================================================================

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    if (event_id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "[PPP] === IP obtenue ===");
        ESP_LOGI(TAG, "[PPP] IP      : " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "[PPP] Netmask : " IPSTR, IP2STR(&event->ip_info.netmask));
        ESP_LOGI(TAG, "[PPP] Gateway : " IPSTR, IP2STR(&event->ip_info.gw));

        xEventGroupSetBits(modem_event_group, PPP_CONNECTED_BIT);

        // --- DIAGNOSTIC : etat route par defaut AVANT notre intervention ---
        struct netif *default_before = netif_default;
        if (default_before) {
            ESP_LOGI(TAG, "[DIAG] Route par defaut AVANT : netif '%c%c%d'",
                     default_before->name[0], default_before->name[1],
                     default_before->num);
        } else {
            ESP_LOGW(TAG, "[DIAG] Route par defaut AVANT : AUCUNE (netif_default=NULL)");
        }

        // --- DIAGNOSTIC : lister toutes les netifs enregistrees ---
        int netif_count = 0;
        for (struct netif *nif = netif_list; nif != NULL; nif = nif->next) {
            ESP_LOGI(TAG, "[DIAG] netif[%d] : '%c%c%d' flags=0x%02x %s",
                     netif_count, nif->name[0], nif->name[1], nif->num,
                     nif->flags,
                     (nif == default_before) ? "<-- DEFAULT" : "");
            netif_count++;
        }
        ESP_LOGI(TAG, "[DIAG] Total netifs : %d", netif_count);

        // =================================================================
        // [v8.4] ANCIEN CODE — COMMENTE (code mort confirme en v8.3-diag)
        //
        // esp_netif_get_netif_impl(ppp_netif) retourne TOUJOURS NULL dans
        // notre configuration (ESP-IDF 5.0.2 + esp_modem PPPoS).
        // Consequence : netif_set_default() n'est jamais appele.
        // Le MQTT fonctionnait quand meme car ESP-IDF met pp1 comme default
        // dans son callback interne "Connected", mais UNIQUEMENT au premier
        // boot. Apres un SMS (bascule COMMAND/DATA), ap2 reste default.
        //
        // Voir synthese debug v8.3 §4 Hypothese 3 pour le detail.
        // =================================================================
        // struct netif *ppp_lwip = (struct netif *)esp_netif_get_netif_impl(ppp_netif);
        // ESP_LOGI(TAG, "[DIAG] esp_netif_get_netif_impl(ppp_netif) = %p (ppp_netif=%p)",
        //          (void *)ppp_lwip, (void *)ppp_netif);
        //
        // if (ppp_lwip) {
        //     netif_set_default(ppp_lwip);
        //     ESP_LOGI(TAG, "[PPP] Route par defaut → PPP ('%c%c%d')",
        //              ppp_lwip->name[0], ppp_lwip->name[1], ppp_lwip->num);
        // } else {
        //     ESP_LOGE(TAG, "[DIAG] ❌ esp_netif_get_netif_impl a retourne NULL !");
        //     ESP_LOGE(TAG, "[DIAG] La route par defaut N'A PAS ete modifiee");
        // }
        // =================================================================

        // =================================================================
        // [v8.4] NOUVEAU CODE — Recherche directe de la netif PPP dans
        // netif_list par son nom ('pp').
        //
        // Pourquoi : esp_netif_get_netif_impl() retourne NULL, donc on
        // cherche la netif lwIP directement. Les logs v8.3-diag confirment
        // que la netif PPP s'appelle 'pp1' dans netif_list.
        //
        // Ce code s'execute a chaque IP_EVENT_PPP_GOT_IP, donc :
        //   - Au premier boot (apres negociation PPP initiale)
        //   - Apres chaque SMS (retour DATA MODE → re-negociation PPP)
        //   - Apres chaque reconnexion PPP (tache ppp_reconnect_task)
        // Dans tous les cas, il force PPP comme route par defaut.
        // =================================================================
        struct netif *ppp_lwip = NULL;
        for (struct netif *nif = netif_list; nif != NULL; nif = nif->next) {
            if (nif->name[0] == 'p' && nif->name[1] == 'p') {
                ppp_lwip = nif;
                break;
            }
        }

        if (ppp_lwip) {
            netif_set_default(ppp_lwip);
            ESP_LOGI(TAG, "[PPP] Route par defaut → PPP ('%c%c%d') [v8.4 netif_list]",
                     ppp_lwip->name[0], ppp_lwip->name[1], ppp_lwip->num);
        } else {
            ESP_LOGE(TAG, "[PPP] ❌ Aucune netif 'pp' trouvee dans netif_list !");
            ESP_LOGE(TAG, "[PPP] La route par defaut N'A PAS ete modifiee");
        }
        // =================================================================

        // --- DIAGNOSTIC : etat route par defaut APRES ---
        struct netif *default_after = netif_default;
        if (default_after) {
            ESP_LOGI(TAG, "[DIAG] Route par defaut APRES : netif '%c%c%d'",
                     default_after->name[0], default_after->name[1],
                     default_after->num);
        } else {
            ESP_LOGW(TAG, "[DIAG] Route par defaut APRES : AUCUNE (netif_default=NULL)");
        }

    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGW(TAG, "[PPP] Connexion PPP perdue !");
        xEventGroupClearBits(modem_event_group, PPP_CONNECTED_BIT);
        // Reveiller la tache de reconnexion PPP
        xEventGroupSetBits(modem_event_group, PPP_RECONNECT_BIT);
    }
}

// =============================================================================
// 4. esp_modem — Connexion PPPoS
// =============================================================================

static esp_modem_dce_t *modem_init_ppp(void)
{
    ESP_LOGI(TAG, "[MODEM] Configuration esp_modem (SIM7070 / PPPoS)...");

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, &ip_event_handler, NULL, NULL));

    esp_modem_dte_config_t dte_config = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_config.uart_config.tx_io_num    = MODEM_UART_TX;
    dte_config.uart_config.rx_io_num    = MODEM_UART_RX;
    dte_config.uart_config.baud_rate    = MODEM_UART_BAUD;
    dte_config.uart_config.rts_io_num   = -1;
    dte_config.uart_config.cts_io_num   = -1;

    ESP_LOGI(TAG, "[MODEM] UART : TX=GPIO%d RX=GPIO%d Baud=%d",
             MODEM_UART_TX, MODEM_UART_RX, MODEM_UART_BAUD);

    esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG(MODEM_APN);

    ESP_LOGI(TAG, "[MODEM] APN : %s", MODEM_APN);

    esp_netif_config_t netif_ppp_config = ESP_NETIF_DEFAULT_PPP();
    ppp_netif = esp_netif_new(&netif_ppp_config);
    if (!ppp_netif) {
        ESP_LOGE(TAG, "[MODEM] ECHEC creation interface PPP !");
        return NULL;
    }
    ESP_LOGI(TAG, "[MODEM] Interface PPP creee");

    esp_modem_dce_t *new_dce = esp_modem_new_dev(
        ESP_MODEM_DCE_SIM7070, &dte_config, &dce_config, ppp_netif);
    if (!new_dce) {
        ESP_LOGE(TAG, "[MODEM] ECHEC creation DCE SIM7070 !");
        return NULL;
    }
    ESP_LOGI(TAG, "[MODEM] DCE SIM7070 cree");

    ESP_LOGI(TAG, "[MODEM] Attente stabilisation modem %d ms...", MODEM_STABILIZE_MS);
    vTaskDelay(pdMS_TO_TICKS(MODEM_STABILIZE_MS));

    // Reveil modem + purge buffer UART avant synchronisation esp_modem.
    // Ces 3 lignes sont indispensables : sans elles, les donnees residuelles
    // du boot modem polluent le flux et empechent l'initialisation PPP.
    // Note : cet acces UART direct est acceptable ici car la tache DTE
    // d'esp_modem n'est pas encore active (pas encore en mode DATA).
    const char at_cmd[] = "AT\r\n";
    uart_write_bytes(UART_NUM_1, at_cmd, sizeof(at_cmd) - 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    uart_flush_input(UART_NUM_1);

    ESP_LOGI(TAG, "[MODEM] Verification communication AT");

    int at_retry = 0;
    bool at_ok = false;

    // Boucle finie : AT_RETRY_MAX essais (20), pulse PWRKEY tous les
    // AT_BEFORE_PWRKEY echecs (6). Soit ~3 cycles PWRKEY avant abandon.
    // Si echec total, modem_init_ppp() retourne NULL et app_main reboote
    // (power cycle complet via pmu_init).
    while (at_retry < AT_RETRY_MAX && !at_ok) {
        esp_err_t sync_err = esp_modem_sync(new_dce);
        if (sync_err == ESP_OK) {
            at_ok = true;
            ESP_LOGI(TAG, "[MODEM] Modem repond aux commandes AT (essai %d)",
                     at_retry + 1);
        } else {
            at_retry++;
            ESP_LOGW(TAG, "[MODEM] Pas de reponse AT (%d/%d)",
                     at_retry, AT_RETRY_MAX);

            if (at_retry % AT_BEFORE_PWRKEY == 0) {
                ESP_LOGW(TAG, "[MODEM] %d echecs consecutifs → pulse PWRKEY",
                         AT_BEFORE_PWRKEY);
                modem_pulse_pwrkey();
            } else {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
    }

    if (!at_ok) {
        ESP_LOGE(TAG, "[MODEM] ECHEC : modem ne repond pas apres %d tentatives !",
                 AT_RETRY_MAX);
        return NULL;
    }

    ESP_LOGI(TAG, "[MODEM] Passage en mode DATA (PPPoS)...");
    esp_err_t err = esp_modem_set_mode(new_dce, ESP_MODEM_MODE_DATA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[MODEM] ECHEC passage mode DATA : %s", esp_err_to_name(err));
        return NULL;
    }

    ESP_LOGI(TAG, "[MODEM] Mode DATA actif — negociation PPP en cours...");
    return new_dce;
}

// =============================================================================
// 5. WiFi SoftAP
// =============================================================================

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event =
            (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "[WIFI] Station connectee — MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                 event->mac[0], event->mac[1], event->mac[2],
                 event->mac[3], event->mac[4], event->mac[5]);

    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event =
            (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGW(TAG, "[WIFI] Station deconnectee — MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                 event->mac[0], event->mac[1], event->mac[2],
                 event->mac[3], event->mac[4], event->mac[5]);
    }
}

static void wifi_init_softap(void)
{
    ESP_LOGI(TAG, "[WIFI] Demarrage SoftAP...");

    ap_netif = esp_netif_create_default_wifi_ap();

    ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap_netif));
    ESP_LOGI(TAG, "[WIFI] DHCP serveur desactive");

    esp_netif_ip_info_t ip_info;
    memset(&ip_info, 0, sizeof(ip_info));
    ip_info.ip.addr      = esp_ip4addr_aton(WIFI_AP_IP);
    ip_info.netmask.addr = esp_ip4addr_aton(WIFI_AP_NETMASK);
    ip_info.gw.addr      = esp_ip4addr_aton(WIFI_AP_IP);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ip_info));
    ESP_LOGI(TAG, "[WIFI] IP statique : %s", WIFI_AP_IP);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {};
    strlcpy((char *)wifi_config.ap.ssid, WIFI_AP_SSID, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len     = strlen(WIFI_AP_SSID);
    wifi_config.ap.channel      = WIFI_AP_CHANNEL;
    wifi_config.ap.password[0]  = '\0';     // Pas de mot de passe
    wifi_config.ap.max_connection = WIFI_AP_MAX_CONN;
    // WiFi ouvert : le lien Waveshare ↔ LilyGo est un réseau privé dédié
    // (1 client max, serre isolée). Le MQTT est protégé par TLS de bout en bout.
    // WPA2 provoquait des erreurs "CCMP mgmt frame used non-zero reserved bit"
    // entre les deux ESP32-S3 (blobs WiFi incompatibles Arduino vs ESP-IDF).
    wifi_config.ap.authmode     = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "[WIFI] SoftAP demarre — SSID: %s | Max conn: %d",
             WIFI_AP_SSID, WIFI_AP_MAX_CONN);

    uint8_t ap_mac[6];
    if (esp_wifi_get_mac(WIFI_IF_AP, ap_mac) == ESP_OK) {
        ESP_LOGI(TAG, "[WIFI] BSSID : %02x:%02x:%02x:%02x:%02x:%02x",
                 ap_mac[0], ap_mac[1], ap_mac[2],
                 ap_mac[3], ap_mac[4], ap_mac[5]);
    }

    esp_netif_ip_info_t running_ip;
    if (esp_netif_get_ip_info(ap_netif, &running_ip) == ESP_OK) {
        ESP_LOGI(TAG, "[WIFI] IP: " IPSTR " | Netmask: " IPSTR " | GW: " IPSTR,
                 IP2STR(&running_ip.ip),
                 IP2STR(&running_ip.netmask),
                 IP2STR(&running_ip.gw));
    }
}

// =============================================================================
// 6. Activation NAPT (DNS + NAT)
// =============================================================================

static void enable_napt(void)
{
    // --- DIAGNOSTIC : route par defaut au moment du NAPT ---
    struct netif *def_napt = netif_default;
    if (def_napt) {
        ESP_LOGI(TAG, "[DIAG-NAPT] Route par defaut actuelle : '%c%c%d'",
                 def_napt->name[0], def_napt->name[1], def_napt->num);
    } else {
        ESP_LOGW(TAG, "[DIAG-NAPT] Route par defaut : AUCUNE");
    }

    esp_netif_dns_info_t dns;
    if (esp_netif_get_dns_info(ppp_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
        esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns);
        ESP_LOGI(TAG, "[NAT] DNS propage vers interface AP : " IPSTR,
                 IP2STR(&dns.ip.u_addr.ip4));
    } else {
        ESP_LOGW(TAG, "[NAT] Impossible de lire le DNS PPP");
    }

    esp_netif_ip_info_t ap_ip;
    if (esp_netif_get_ip_info(ap_netif, &ap_ip) == ESP_OK) {
        ip_napt_enable(ap_ip.ip.addr, 1);
        ESP_LOGI(TAG, "[NAT] NAPT active sur interface WiFi AP (" IPSTR ")",
                 IP2STR(&ap_ip.ip));
    } else {
        ESP_LOGE(TAG, "[NAT] Impossible de lire l'IP de l'AP pour NAPT");
    }
}

// =============================================================================
// Forward declarations — fonctions SMS utilisées par udp_bridge_task
// (définies plus bas dans la section 7)
// =============================================================================
static bool sms_validate_number_format(const char *number);
static bool sms_check_whitelist(const char *number);
static int  sms_table_add(const char *request_id, const char *to, const char *text,
                           const char *source, int ttl_s);

// =============================================================================
// 6b. UDP Bridge — Fonctions utilitaires + tâche
//
// Communication Waveshare ↔ LilyGo par UDP unicast.
// Remplace le heartbeat HTTP et la communication SMS HTTP.
//
// Protocole (4 paquets, format pipe-séparé) :
//   Waveshare → LilyGo : HB, SMS|number|text
//   LilyGo → Waveshare : STATE|0 ou STATE|1, ACK
// =============================================================================

// --- Détermine si la LilyGo peut accepter un ordre SMS ---
static bool can_accept_sms(void)
{
    // PPP connecté ?
    EventBits_t bits = xEventGroupGetBits(modem_event_group);
    if (!(bits & PPP_CONNECTED_BIT)) return false;

    // Place dans la queue ?
    if (uxQueueSpacesAvailable(sms_queue) == 0) return false;

    // Rate limit écoulé ? (5 min entre deux envois, réussis ou pas)
    int64_t now_ms = esp_timer_get_time() / 1000;
    xSemaphoreTake(sms_mutex, portMAX_DELAY);
    int64_t last_sent = last_sms_sent_ms;
    xSemaphoreGive(sms_mutex);

    if (last_sent > 0 && (now_ms - last_sent) < SMS_RATE_LIMIT_MS)
        return false;

    // LilyGo pas en train de traiter un SMS ?
    if (sms_processing) return false;

    return true;
}

// --- Envoie STATE|0 ou STATE|1 vers la Waveshare ---
static void send_udp_state(bool can_accept)
{
    if (udp_sock < 0) return;

    char buf[8];
    snprintf(buf, sizeof(buf), "STATE|%d", can_accept ? 1 : 0);

    sendto(udp_sock, buf, strlen(buf), 0,
           (struct sockaddr *)&waveshare_addr, sizeof(waveshare_addr));

    ESP_LOGI(TAG, "[UDP] Etat envoyé : %s", buf);
}

// --- Envoie ACK vers la Waveshare ---
static void send_udp_ack(void)
{
    if (udp_sock < 0) return;

    sendto(udp_sock, "ACK", 3, 0,
           (struct sockaddr *)&waveshare_addr, sizeof(waveshare_addr));

    ESP_LOGI(TAG, "[UDP] ACK envoyé");
}

// =============================================================================
// 6c. Test de connectivite Internet par ping ICMP
//
// Verifie que Internet est REELLEMENT accessible (pas seulement que PPP est UP).
// Utilise par udp_bridge_task pour detecter les "PPP fantomes" (PPP_CONNECTED_BIT
// UP mais routage IP casse, typiquement apres glitch GSM).
//
// Fonction synchrone : lance un ping, attend le resultat (timeout 5s), retourne
// true si succes, false sinon. Utilise un semaphore pour synchroniser avec le
// callback de esp_ping (qui s'execute dans une tache interne).
// =============================================================================

// Variables partagees entre ping_test() et ses callbacks
static SemaphoreHandle_t ping_done_sem    = NULL;
static volatile bool     ping_last_result = false;

// Callback : succes du ping
static void ping_success_cb(esp_ping_handle_t hdl, void *args)
{
    ping_last_result = true;
    if (ping_done_sem) xSemaphoreGive(ping_done_sem);
}

// Callback : echec du ping (timeout ou erreur reseau)
static void ping_timeout_cb(esp_ping_handle_t hdl, void *args)
{
    ping_last_result = false;
    if (ping_done_sem) xSemaphoreGive(ping_done_sem);
}

// Callback : fin de session (un seul ping envoye)
static void ping_end_cb(esp_ping_handle_t hdl, void *args)
{
    // Rien a faire : le resultat est deja remonte par success_cb ou timeout_cb.
    // On laisse esp_ping nettoyer sa session.
}

// --- Lance un ping ICMP et retourne true si reponse recue dans le timeout ---
static bool ping_test(const char *target_ip)
{
    // Resoudre l'adresse cible (target_ip est une IP litterale, donc instantane)
    ip_addr_t target_addr;
    if (ipaddr_aton(target_ip, &target_addr) == 0) {
        ESP_LOGE(TAG, "[PING] Adresse invalide : %s", target_ip);
        return false;
    }

    // Creer le semaphore (lazy init, une seule fois)
    if (!ping_done_sem) {
        ping_done_sem = xSemaphoreCreateBinary();
        if (!ping_done_sem) {
            ESP_LOGE(TAG, "[PING] Echec creation semaphore");
            return false;
        }
    }

    // Configuration du ping (un seul envoi, timeout 5s)
    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.target_addr = target_addr;
    config.count       = 1;
    config.timeout_ms  = PING_TIMEOUT_MS;
    config.interval_ms = 0;  // Sans objet pour count=1

    esp_ping_callbacks_t cbs = {
        .cb_args        = NULL,
        .on_ping_success = ping_success_cb,
        .on_ping_timeout = ping_timeout_cb,
        .on_ping_end    = ping_end_cb
    };

    esp_ping_handle_t ping_hdl;
    esp_err_t err = esp_ping_new_session(&config, &cbs, &ping_hdl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[PING] Echec creation session ping vers %s : %s",
                 target_ip, esp_err_to_name(err));
        return false;
    }

    // Reset du resultat avant lancement
    ping_last_result = false;

    // Lancer le ping
    err = esp_ping_start(ping_hdl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[PING] Echec lancement ping : %s", esp_err_to_name(err));
        esp_ping_delete_session(ping_hdl);
        return false;
    }

    // Attendre le resultat (timeout = PING_TIMEOUT_MS + marge 2s pour la session)
    bool got_result = (xSemaphoreTake(ping_done_sem,
                                      pdMS_TO_TICKS(PING_TIMEOUT_MS + 2000)) == pdTRUE);

    // Liberer la session (cleanup)
    esp_ping_stop(ping_hdl);
    esp_ping_delete_session(ping_hdl);

    if (!got_result) {
        ESP_LOGW(TAG, "[PING] Timeout interne pour %s (semaphore non libere)",
                 target_ip);
        return false;
    }

    return ping_last_result;
}

// --- Cycle complet de verification : essaie 3 cibles avant de conclure ---
// Retourne true si AU MOINS UNE des 3 cibles repond, false si toutes echouent.
// Loggue chaque tentative.
static bool ping_cycle_check(void)
{
    ESP_LOGI(TAG, "[PING] Cycle de verification connectivite Internet");

    // Cible 1 : Cloudflare (regime nominal)
    if (ping_test(PING_TARGET_PRIMARY)) {
        ESP_LOGI(TAG, "[PING] %s repond → Internet OK", PING_TARGET_PRIMARY);
        return true;
    }
    ESP_LOGW(TAG, "[PING] %s ne repond pas — essai cible suivante",
             PING_TARGET_PRIMARY);

    // Cible 2 : Google
    if (ping_test(PING_TARGET_FALLBACK_1)) {
        ESP_LOGI(TAG, "[PING] %s repond → Internet OK", PING_TARGET_FALLBACK_1);
        return true;
    }
    ESP_LOGW(TAG, "[PING] %s ne repond pas — essai cible suivante",
             PING_TARGET_FALLBACK_1);

    // Cible 3 : Quad9
    if (ping_test(PING_TARGET_FALLBACK_2)) {
        ESP_LOGI(TAG, "[PING] %s repond → Internet OK", PING_TARGET_FALLBACK_2);
        return true;
    }

    ESP_LOGE(TAG, "[PING] AUCUNE des 3 cibles ne repond → Internet KO");
    return false;
}

// --- Tâche UDP Bridge ---
// Écoute les paquets UDP de la Waveshare (HB, SMS).
// Envoie le STATE toutes les 30s.
static void udp_bridge_task(void *arg)
{
    ESP_LOGI(TAG, "[UDP] Tâche UDP Bridge démarrée — écoute port %d", UDP_LOCAL_PORT);

    // Timeout reception UDP : configure une seule fois (valeur fixe 1s)
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(udp_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int64_t last_state_ms = 0;
    int64_t last_batt_check_ms = 0;

    // --- Surveillance PPP (relance forcee + reboot) ---
    // ppp_down_since_ms : timestamp (ms) de la premiere constatation que PPP est down.
    //                     0 = PPP est UP (ou jamais constate down).
    // last_reconnect_trigger_ms : timestamp du dernier reveil force de
    //                              ppp_reconnect_task. Sert a espacer les reveils
    //                              de PPP_DOWN_RELAUNCH_MS minimum.
    int64_t ppp_down_since_ms         = 0;
    int64_t last_reconnect_trigger_ms = 0;

    // --- Test connectivite Internet (ping ICMP) — Machine d'etat ---
    //
    // 3 etats possibles :
    //   PING_IDLE      : surveillance normale, on attend la prochaine echeance (40 min).
    //   PING_WAIT_PPP  : on a force une renegociation PPP, on attend son retour (max 90s).
    //   PING_RETEST    : PPP est revenu, on doit relancer un ping de validation au
    //                    prochain tour (avec une petite stabilisation 5s).
    //
    // Aucun bloc bloquant long : a chaque tour de boucle (~1 fois/seconde),
    // on evalue l'etat et les transitions. Toutes les autres surveillances
    // (UDP, batterie, heartbeat Waveshare, watchdog PPP) continuent de tourner
    // pendant les phases de recuperation ping.
    enum PingState {
        PING_IDLE,
        PING_WAIT_PPP,
        PING_RETEST
    };

    PingState ping_state          = PING_IDLE;
    int64_t   ping_state_since_ms = 0;       // Timestamp d'entree dans l'etat courant
    int       ping_failure_count  = 0;       // Cycles consecutifs ou la verification echoue

    // boot_time_ms : timestamp de demarrage de la tache, pour le delai de grace.
    // last_ping_success_ms : timestamp du dernier ping reussi (ou de demarrage si jamais).
    //                       Sert a calculer "il y a combien de temps un ping a reussi".
    int64_t boot_time_ms         = esp_timer_get_time() / 1000;
    int64_t last_ping_success_ms = boot_time_ms;

    while (true) {
        // --- recvfrom avec timeout 1s ---
        char buf[256];
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);

        int len = recvfrom(udp_sock, buf, sizeof(buf) - 1, 0,
                           (struct sockaddr *)&from_addr, &from_len);

        if (len > 0) {
            buf[len] = '\0';

            // ── HB (heartbeat Waveshare) ─────────────────────────────
            if (len == 2 && buf[0] == 'H' && buf[1] == 'B') {
                portENTER_CRITICAL(&activity_spinlock);
                last_activity_ms = esp_timer_get_time() / 1000;
                portEXIT_CRITICAL(&activity_spinlock);

                // Waveshare de retour : autoriser une nouvelle alerte SMS si elle disparait
                if (alerte_heartbeat_sms_envoye) {
                    alerte_heartbeat_sms_envoye = false;
                    ESP_LOGI(TAG, "[UDP] Heartbeat de retour — alerte SMS rearmee");
                }

                ESP_LOGI(TAG, "[UDP] Heartbeat reçu de la Waveshare");
            }
            // ── SMS|number|text ──────────────────────────────────────
            else if (len >= 5 && strncmp(buf, "SMS|", 4) == 0) {

                // Vérifier disponibilité
                if (!can_accept_sms()) {
                    ESP_LOGW(TAG, "[UDP] Ordre SMS reçu mais LilyGo non disponible — ignoré");
                } else {
                    // Parser : extraire number et text
                    char *payload = buf + 4;  // après "SMS|"
                    char *sep = strchr(payload, '|');

                    if (!sep || sep == payload) {
                        ESP_LOGW(TAG, "[UDP] Ordre SMS malformé — ignoré");
                    } else {
                        *sep = '\0';
                        char *number = payload;
                        char *text = sep + 1;

                        // Validation du contenu (protection contre paquet corrompu)
                        // En cas d'echec : ignoré silencieusement.
                        // La Waveshare timeout apres 3 min, retry, puis abandonne.
                        if (!sms_validate_number_format(number)) {
                            ESP_LOGW(TAG, "[UDP] Numéro invalide — ignoré");
                        } else if (!sms_check_whitelist(number)) {
                            ESP_LOGW(TAG, "[UDP] Numéro hors whitelist — ignoré");
                        } else if (text[0] == '\0' || strlen(text) > SMS_TEXT_MAX_LEN) {
                            ESP_LOGW(TAG, "[UDP] Texte vide ou trop long — ignoré");
                        } else {
                            ESP_LOGI(TAG, "[UDP] Ordre SMS reçu — dest:%s texte:%.40s%s",
                                     number, text, strlen(text) > 40 ? "..." : "");

                            // Créer l'entrée dans sms_table
                            xSemaphoreTake(sms_mutex, portMAX_DELAY);

                            // Générer un request_id local
                            char req_id[33];
                            snprintf(req_id, sizeof(req_id), "udp_%lld",
                                     (long long)(esp_timer_get_time() / 1000));

                            int idx = sms_table_add(req_id, number, text, "waveshare",
                                                    SMS_TTL_DEFAULT);

                            if (idx >= 0) {
                                sms_table[idx].from_udp = true;
                                xSemaphoreGive(sms_mutex);

                                // Envoyer dans la queue FreeRTOS
                                if (xQueueSend(sms_queue, &idx, 0) != pdTRUE) {
                                    ESP_LOGE(TAG, "[UDP] Queue SMS pleine — slot %d perdu", idx);
                                    xSemaphoreTake(sms_mutex, portMAX_DELAY);
                                    sms_table[idx].status = SMS_STATUS_FAILED;
                                    strlcpy(sms_table[idx].error_code, "QUEUE_FULL",
                                            sizeof(sms_table[idx].error_code));
                                    xSemaphoreGive(sms_mutex);
                                } else {
                                    ESP_LOGI(TAG, "[UDP] SMS en queue — slot %d req_id=%s",
                                             idx, req_id);
                                }
                            } else {
                                xSemaphoreGive(sms_mutex);
                                ESP_LOGW(TAG, "[UDP] Table SMS pleine — ordre ignoré");
                            }
                        }
                    }
                }
            }
            // ── Paquet inconnu ───────────────────────────────────────
            else {
                ESP_LOGW(TAG, "[UDP] Paquet inconnu reçu (%d octets): %s", len, buf);
            }
        }

        // --- Envoi STATE périodique (toutes les 30s) ---
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - last_state_ms >= STATE_INTERVAL_MS) {
            send_udp_state(can_accept_sms());
            last_state_ms = now_ms;
        }

        // ─── Surveillance PPP : relance forcee + reboot si panne durable ─────
        // Mecanisme independant de ppp_reconnect_task qui peut rester endormie
        // si l'evenement IP_EVENT_PPP_LOST_IP n'a pas ete emis (cas observe :
        // bascule SMS terminee mais PPP n'a jamais renegocie une IP).
        //
        // Logique :
        //   - PPP UP   : reset des compteurs
        //   - PPP DOWN depuis ≥ 5 min : on positionne PPP_RECONNECT_BIT pour
        //     reveiller ppp_reconnect_task. On NE TESTE PAS si elle dort
        //     (par robustesse : ne pas dependre d'un etat interne).
        //     On respace les reveils de PPP_DOWN_RELAUNCH_MS minimum.
        //   - PPP DOWN depuis ≥ 2h : reboot complet (independant du heartbeat
        //     Waveshare : la Waveshare est stable, on peut rebooter seul).
        {
            EventBits_t bits_ppp = xEventGroupGetBits(modem_event_group);
            bool ppp_up = (bits_ppp & PPP_CONNECTED_BIT) != 0;

            if (ppp_up) {
                // PPP est UP : reset des compteurs
                if (ppp_down_since_ms != 0) {
                    ESP_LOGI(TAG, "[PPP-WATCH] PPP de retour UP — surveillance reinitialisee");
                }
                ppp_down_since_ms         = 0;
                last_reconnect_trigger_ms = 0;
            } else {
                // PPP est DOWN
                if (ppp_down_since_ms == 0) {
                    // Premiere constatation : memoriser le timestamp
                    ppp_down_since_ms = now_ms;
                    ESP_LOGW(TAG, "[PPP-WATCH] PPP detecte DOWN — surveillance armee");
                }

                int64_t down_duration_ms = now_ms - ppp_down_since_ms;

                // --- Seuil 1 : reveil force toutes les 5 min ---
                bool first_trigger    = (last_reconnect_trigger_ms == 0);
                bool relaunch_due     = (now_ms - last_reconnect_trigger_ms) >= PPP_DOWN_RELAUNCH_MS;
                bool threshold_reached = (down_duration_ms >= PPP_DOWN_RELAUNCH_MS);

                if (threshold_reached && (first_trigger || relaunch_due)) {
                    ESP_LOGW(TAG, "[PPP-WATCH] PPP down depuis %lld min — reveil force ppp_reconnect_task",
                             (long long)(down_duration_ms / 60000));
                    xEventGroupSetBits(modem_event_group, PPP_RECONNECT_BIT);
                    last_reconnect_trigger_ms = now_ms;
                }

                // --- Seuil 2 : reboot apres 2h de PPP down ---
                if (WATCHDOG_REBOOT_ENABLED && down_duration_ms >= PPP_DOWN_REBOOT_MS) {
                    ESP_LOGE(TAG, "[PPP-WATCH] ════════════════════════════════════");
                    ESP_LOGE(TAG, "[PPP-WATCH] ❌ PPP down depuis %lld min → REBOOT",
                             (long long)(down_duration_ms / 60000));
                    ESP_LOGE(TAG, "[PPP-WATCH] ════════════════════════════════════");
                    vTaskDelay(pdMS_TO_TICKS(1000));  // Laisser les logs sortir
                    esp_restart();
                }
            }
        }

        // ─── Test connectivite Internet par ping ICMP — Machine d'etat ──────
        // Detecte les "PPP fantomes" sans jamais bloquer la boucle plus longtemps
        // qu'un ping ICMP (~5-15s pour un cycle complet de 1 a 3 cibles).
        // Pendant les phases d'attente du retour PPP, la boucle continue de
        // traiter UDP, surveillance batterie, heartbeat Waveshare, watchdog PPP.
        //
        // Ne s'execute QUE si PPP est UP (sinon le mecanisme PPP-WATCH au-dessus
        // s'en occupe deja). Premier ping retarde de PING_BOOT_GRACE_MS (15 min)
        // apres demarrage de la tache pour ne pas perturber le boot.
        {
            EventBits_t bits_for_ping = xEventGroupGetBits(modem_event_group);
            bool ppp_up_for_ping = (bits_for_ping & PPP_CONNECTED_BIT) != 0;

            switch (ping_state) {

            // ─── Etat IDLE : surveillance normale ─────────────────────────────
            case PING_IDLE: {
                if (!ppp_up_for_ping) break;  // PPP down : geree par PPP-WATCH

                // Delai de grace au boot (pas de ping pendant les premieres minutes)
                if ((now_ms - boot_time_ms) < PING_BOOT_GRACE_MS) break;

                // Echeance du prochain ping atteinte ?
                if ((now_ms - last_ping_success_ms) < PING_CHECK_INTERVAL_MS) break;

                // C'est l'heure : faire un cycle de verification
                ESP_LOGI(TAG, "[PING-WATCH] ════════════════════════════════════");
                ESP_LOGI(TAG, "[PING-WATCH] Verification connectivite Internet");

                bool internet_ok = ping_cycle_check();

                if (internet_ok) {
                    // Internet repond : reset compteur, reste IDLE, prochaine verif dans 40 min
                    if (ping_failure_count > 0) {
                        ESP_LOGI(TAG, "[PING-WATCH] Internet retabli (compteur reset)");
                    }
                    ping_failure_count   = 0;
                    last_ping_success_ms = now_ms;
                } else {
                    // Aucune des 3 cibles n'a repondu : PPP fantome probable
                    ESP_LOGW(TAG, "[PING-WATCH] Internet KO malgre PPP UP — forcage renegociation PPP");
                    xEventGroupSetBits(modem_event_group, PPP_RECONNECT_BIT);

                    // Transition vers WAIT_PPP : on attend que PPP revienne
                    ping_state          = PING_WAIT_PPP;
                    ping_state_since_ms = now_ms;
                    ESP_LOGI(TAG, "[PING-WATCH] Etat → WAIT_PPP (attente max 90s)");
                }
                ESP_LOGI(TAG, "[PING-WATCH] ════════════════════════════════════");
                break;
            }

            // ─── Etat WAIT_PPP : attente non bloquante du retour PPP ──────────
            case PING_WAIT_PPP: {
                int64_t wait_duration_ms = now_ms - ping_state_since_ms;

                // PPP est revenu UP ?
                if (ppp_up_for_ping) {
                    // On laisse 5s de stabilisation au NAPT/DNS avant de retester.
                    // Le critere : PPP doit etre UP depuis au moins 5s
                    // (on utilise le timestamp d'entree dans l'etat comme reference,
                    // car PPP est passe UP entre temps mais on ignore quand exactement).
                    // Approximation suffisante pour notre besoin.
                    if (wait_duration_ms >= 5000) {
                        ESP_LOGI(TAG, "[PING-WATCH] PPP revenu et stabilise → etat RETEST");
                        ping_state          = PING_RETEST;
                        ping_state_since_ms = now_ms;
                    }
                    break;
                }

                // PPP toujours pas revenu : timeout 90s ?
                if (wait_duration_ms >= 90000) {
                    ESP_LOGW(TAG, "[PING-WATCH] PPP toujours pas revenu apres 90s — echec");
                    ping_failure_count++;
                    ESP_LOGW(TAG, "[PING-WATCH] Compteur d'echecs : %d/%d",
                             ping_failure_count, PING_MAX_FAILURES);

                    // Reboot si trop d'echecs consecutifs
                    if (WATCHDOG_REBOOT_ENABLED && ping_failure_count >= PING_MAX_FAILURES) {
                        ESP_LOGE(TAG, "[PING-WATCH] ════════════════════════════════════");
                        ESP_LOGE(TAG, "[PING-WATCH] ❌ %d cycles ping consecutifs KO → REBOOT",
                                 ping_failure_count);
                        ESP_LOGE(TAG, "[PING-WATCH] ════════════════════════════════════");
                        vTaskDelay(pdMS_TO_TICKS(1000));
                        esp_restart();
                    }

                    // Pas de reboot : on retourne en IDLE, prochaine verif dans 40 min
                    // (pas la peine de reessayer tout de suite, ca echouera pareil)
                    last_ping_success_ms = now_ms;  // Reset du timer pour respecter l'intervalle
                    ping_state           = PING_IDLE;
                }
                break;
            }

            // ─── Etat RETEST : ping de validation apres renegociation ─────────
            case PING_RETEST: {
                ESP_LOGI(TAG, "[PING-WATCH] Re-test connectivite apres renegociation PPP");

                bool retest_ok = ping_cycle_check();

                if (retest_ok) {
                    ESP_LOGI(TAG, "[PING-WATCH] ✅ Renegociation efficace, Internet OK");
                    ping_failure_count   = 0;
                    last_ping_success_ms = now_ms;
                } else {
                    ESP_LOGW(TAG, "[PING-WATCH] ❌ Renegociation inefficace, Internet toujours KO");
                    ping_failure_count++;
                    ESP_LOGW(TAG, "[PING-WATCH] Compteur d'echecs : %d/%d",
                             ping_failure_count, PING_MAX_FAILURES);

                    // Reboot si trop d'echecs consecutifs
                    if (WATCHDOG_REBOOT_ENABLED && ping_failure_count >= PING_MAX_FAILURES) {
                        ESP_LOGE(TAG, "[PING-WATCH] ════════════════════════════════════");
                        ESP_LOGE(TAG, "[PING-WATCH] ❌ %d cycles ping consecutifs KO → REBOOT",
                                 ping_failure_count);
                        ESP_LOGE(TAG, "[PING-WATCH] ════════════════════════════════════");
                        vTaskDelay(pdMS_TO_TICKS(1000));
                        esp_restart();
                    }

                    // Pas de reboot : retour IDLE, prochaine verif dans 40 min
                    last_ping_success_ms = now_ms;
                }

                // Quel que soit le resultat, retour en IDLE pour le prochain cycle
                ping_state = PING_IDLE;
                break;
            }
            }
        }

        // --- Alerte SMS si Waveshare muette depuis trop longtemps ---
        // Independant du PPP : on veut savoir que la Waveshare ne repond plus,
        // meme si la connexion internet fonctionne.
        if (ALERTE_ABSENCE_HEARTBEAT_SMS_ENABLED && !alerte_heartbeat_sms_envoye) {
            portENTER_CRITICAL(&activity_spinlock);
            int64_t inactivite_ms = now_ms - last_activity_ms;
            portEXIT_CRITICAL(&activity_spinlock);

            if (inactivite_ms > ALERTE_ABSENCE_HEARTBEAT_DELAI_MS) {
                ESP_LOGW(TAG, "[UDP] ⚠️ Waveshare muette depuis %lld min — envoi SMS alerte",
                         (long long)(inactivite_ms / 60000));

                xSemaphoreTake(sms_mutex, portMAX_DELAY);

                char req_id[33];
                snprintf(req_id, sizeof(req_id), "hb_alerte_%lld",
                         (long long)(esp_timer_get_time() / 1000));

                char alerte_text[161];
                snprintf(alerte_text, sizeof(alerte_text),
                         "ALERTE serre : Waveshare muette depuis %lld min",
                         (long long)(inactivite_ms / 60000));

                int idx = sms_table_add(req_id, sms_whitelist[0], alerte_text,
                                        "lilygo_watchdog", SMS_TTL_DEFAULT);

                if (idx >= 0) {
                    sms_table[idx].from_udp = false;
                    xSemaphoreGive(sms_mutex);

                    if (xQueueSend(sms_queue, &idx, 0) == pdTRUE) {
                        alerte_heartbeat_sms_envoye = true;
                        ESP_LOGI(TAG, "[UDP] SMS alerte Waveshare en queue (slot %d)", idx);
                    } else {
                        ESP_LOGW(TAG, "[UDP] Queue pleine — SMS alerte non envoye");
                    }
                } else {
                    xSemaphoreGive(sms_mutex);
                    ESP_LOGW(TAG, "[UDP] Table pleine — SMS alerte non envoye");
                }
            }
        }

        // --- Alerte SMS batterie : surveillance coupure secteur ---
        // Verifie toutes les 30s (timestamp dedie, independant du STATE).
        if (ALERTE_BATTERIE_ENABLED && now_ms - last_batt_check_ms >= 30000) {
            last_batt_check_ms = now_ms;

            int bat_mv = pmu.getBattVoltage();
            int bat_pct = (bat_mv - 3000) * 100 / (4200 - 3000);
            if (bat_pct < 0)   bat_pct = 0;
            if (bat_pct > 100) bat_pct = 100;

            // Batterie passe sous le seuil bas → alerte coupure secteur
            if (!alerte_batterie_basse_envoyee && bat_pct < ALERTE_BATTERIE_SEUIL_BAS_POURCENT) {
                ESP_LOGW(TAG, "[BATT] ⚠️ Batterie %d%% (<%d%%) — coupure secteur probable",
                         bat_pct, ALERTE_BATTERIE_SEUIL_BAS_POURCENT);

                xSemaphoreTake(sms_mutex, portMAX_DELAY);

                char req_id[33];
                snprintf(req_id, sizeof(req_id), "batt_lo_%lld",
                         (long long)(esp_timer_get_time() / 1000));

                char alerte_text[161];
                snprintf(alerte_text, sizeof(alerte_text),
                         "ALERTE serre : batterie %d%% coupure secteur probable",
                         bat_pct);

                int idx = sms_table_add(req_id, sms_whitelist[0], alerte_text,
                                        "lilygo_batterie", SMS_TTL_DEFAULT);

                if (idx >= 0) {
                    sms_table[idx].from_udp = false;
                    xSemaphoreGive(sms_mutex);

                    if (xQueueSend(sms_queue, &idx, 0) == pdTRUE) {
                        alerte_batterie_basse_envoyee = true;
                        ESP_LOGI(TAG, "[BATT] SMS alerte batterie en queue (slot %d)", idx);
                    } else {
                        ESP_LOGW(TAG, "[BATT] Queue pleine — SMS alerte batterie non envoye");
                    }
                } else {
                    xSemaphoreGive(sms_mutex);
                    ESP_LOGW(TAG, "[BATT] Table pleine — SMS alerte batterie non envoye");
                }
            }

            // Batterie remonte au-dessus du seuil haut → secteur retabli
            if (alerte_batterie_basse_envoyee && bat_pct >= ALERTE_BATTERIE_SEUIL_HAUT_POURCENT) {
                alerte_batterie_retablie_a_envoyer = true;
            }

            // SMS de retablissement en attente → reessayer si possible
            if (alerte_batterie_retablie_a_envoyer && can_accept_sms()) {
                ESP_LOGI(TAG, "[BATT] ✅ Batterie %d%% (>=%d%%) secteur retabli",
                         bat_pct, ALERTE_BATTERIE_SEUIL_HAUT_POURCENT);

                xSemaphoreTake(sms_mutex, portMAX_DELAY);

                char req_id[33];
                snprintf(req_id, sizeof(req_id), "batt_hi_%lld",
                         (long long)(esp_timer_get_time() / 1000));

                char alerte_text[161];
                snprintf(alerte_text, sizeof(alerte_text),
                         "Serre : secteur retabli batterie %d%%", bat_pct);

                int idx = sms_table_add(req_id, sms_whitelist[0], alerte_text,
                                        "lilygo_batterie", SMS_TTL_DEFAULT);

                if (idx >= 0) {
                    sms_table[idx].from_udp = false;
                    xSemaphoreGive(sms_mutex);

                    if (xQueueSend(sms_queue, &idx, 0) == pdTRUE) {
                        alerte_batterie_basse_envoyee = false;
                        alerte_batterie_retablie_a_envoyer = false;
                        ESP_LOGI(TAG, "[BATT] SMS retablissement en queue (slot %d)", idx);
                    } else {
                        ESP_LOGW(TAG, "[BATT] Queue pleine — SMS retablissement reessaye dans 30s");
                    }
                } else {
                    xSemaphoreGive(sms_mutex);
                    ESP_LOGW(TAG, "[BATT] Table pleine — SMS retablissement reessaye dans 30s");
                }
            }
        }
    }
}

// =============================================================================
// 7. Service SMS — Serveur HTTP + Queue + Table de statuts
// =============================================================================

// --- Helpers de validation ---

// Verifie le format international : '+' suivi de 7 a 15 chiffres (norme E.164)
static bool sms_validate_number_format(const char *number)
{
    if (!number || number[0] != '+') return false;

    int digits = 0;
    for (int i = 1; number[i] != '\0'; i++) {
        if (number[i] < '0' || number[i] > '9') return false;
        digits++;
    }

    return (digits >= 7 && digits <= 15);
}

// Verifie que le numero est dans la whitelist
static bool sms_check_whitelist(const char *number)
{
    for (size_t i = 0; i < SMS_WHITELIST_SIZE; i++) {
        if (strcmp(number, sms_whitelist[i]) == 0) return true;
    }
    return false;
}

// --- Table de statuts ---

// sms_table_find : commenté (v9.0) — utilisé uniquement par les handlers HTTP
// Conservé pour réactivation future.
#if 0
// Cherche un request_id dans la table. Retourne l'index ou -1 si absent.
// ATTENTION : doit etre appele avec sms_mutex pris.
static int sms_table_find(const char *request_id)
{
    for (int i = 0; i < SMS_TABLE_SIZE; i++) {
        if (sms_table[i].used && strcmp(sms_table[i].request_id, request_id) == 0) {
            return i;
        }
    }
    return -1;
}
#endif

// Ajoute une entree dans la table (ecrasement circulaire des slots terminaux).
// ATTENTION : doit etre appele avec sms_mutex pris.
// Retourne l'index de l'entree creee, ou -1 si le slot est non terminal (PENDING/SENDING).
static int sms_table_add(const char *request_id, const char *to, const char *text,
                          const char *source, int ttl_s)
{
    int idx = sms_table_next;
    sms_entry_t *entry = &sms_table[idx];

    // Si le slot est occupe et non terminal (PENDING/SENDING), refuser l'ecrasement
    if (entry->used) {
        if (entry->status == SMS_STATUS_PENDING || entry->status == SMS_STATUS_SENDING) {
            ESP_LOGW(TAG, "[SMS-TABLE] Slot %d non terminal (status=%s) — refus ecrasement",
                     idx, sms_status_to_str(entry->status));
            return -1;
        }
        ESP_LOGI(TAG, "[SMS-TABLE] Slot %d recycle (ancien request_id=%s status=%s)",
                 idx, entry->request_id, sms_status_to_str(entry->status));
    }

    memset(entry, 0, sizeof(sms_entry_t));
    entry->used = true;
    strlcpy(entry->request_id, request_id, sizeof(entry->request_id));
    strlcpy(entry->to, to, sizeof(entry->to));
    strlcpy(entry->text, text, sizeof(entry->text));
    strlcpy(entry->source, source, sizeof(entry->source));
    entry->ttl_s = ttl_s;
    entry->created_ms = esp_timer_get_time() / 1000;  // µs → ms
    entry->status = SMS_STATUS_PENDING;

    ESP_LOGI(TAG, "[SMS-TABLE] Slot %d : request_id=%s to=%s ttl=%ds status=PENDING",
             idx, request_id, to, ttl_s);

    // Avancer l'index circulaire
    sms_table_next = (sms_table_next + 1) % SMS_TABLE_SIZE;

    return idx;
}

// =============================================================================
// SECTION HTTP SMS — MISE EN COMMENTAIRE (v4.3 — remplacé par UDP)
// Le serveur HTTP et ses handlers sont conservés pour réactivation future.
// =============================================================================
#if 0  // ── DEBUT BLOC HTTP SMS COMMENTÉ ──

// --- Helper pour envoyer une reponse JSON ---
static esp_err_t sms_send_json_response(httpd_req_t *req, int http_status,
                                         const char *status, const char *request_id,
                                         const char *reason)
{
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    cJSON_AddStringToObject(resp, "status", status);
    if (request_id) {
        cJSON_AddStringToObject(resp, "request_id", request_id);
    }
    if (reason) {
        cJSON_AddStringToObject(resp, "reason", reason);
    }

    const char *json_str = cJSON_PrintUnformatted(resp);
    if (!json_str) {
        cJSON_Delete(resp);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON print failed");
        return ESP_FAIL;
    }

    httpd_resp_set_status(req, (http_status == 200) ? "200 OK" :
                                (http_status == 400) ? "400 Bad Request" :
                                (http_status == 404) ? "404 Not Found" :
                                "422 Unprocessable Entity");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t err = httpd_resp_sendstr(req, json_str);

    free((void *)json_str);
    cJSON_Delete(resp);
    return err;
}

// --- Handler POST /sms ---
static esp_err_t sms_post_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "[SMS] ── POST /sms recu (content_len=%d) ──", req->content_len);

    // --- Limiter la taille du body ---
    if (req->content_len <= 0 || req->content_len > 512) {
        ESP_LOGW(TAG, "[SMS] Body invalide (len=%d) → REJECTED", req->content_len);
        return sms_send_json_response(req, 400, "REJECTED", NULL, "INVALID_BODY");
    }

    // --- Lire le body (boucle pour gerer les lectures partielles) ---
    char buf[513] = {0};
    int total_received = 0;
    while (total_received < req->content_len) {
        int received = httpd_req_recv(req, buf + total_received,
                                       req->content_len - total_received);
        if (received <= 0) {
            ESP_LOGW(TAG, "[SMS] Erreur lecture body (recu %d/%d) → REJECTED",
                     total_received, req->content_len);
            return sms_send_json_response(req, 400, "REJECTED", NULL, "READ_ERROR");
        }
        total_received += received;
    }
    buf[total_received] = '\0';

    ESP_LOGI(TAG, "[SMS] Body (%d octets) : %s", total_received, buf);

    // --- Parser le JSON ---
    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        ESP_LOGW(TAG, "[SMS] JSON invalide → REJECTED");
        return sms_send_json_response(req, 400, "REJECTED", NULL, "INVALID_JSON");
    }

    // --- Extraire les champs ---
    cJSON *j_to         = cJSON_GetObjectItemCaseSensitive(json, "to");
    cJSON *j_text       = cJSON_GetObjectItemCaseSensitive(json, "text");
    cJSON *j_request_id = cJSON_GetObjectItemCaseSensitive(json, "request_id");
    cJSON *j_ttl        = cJSON_GetObjectItemCaseSensitive(json, "ttl_s");
    cJSON *j_source     = cJSON_GetObjectItemCaseSensitive(json, "source");

    // --- Valider les champs obligatoires ---
    if (!cJSON_IsString(j_to) || !j_to->valuestring[0]) {
        ESP_LOGW(TAG, "[SMS] Champ 'to' manquant ou vide → REJECTED/MISSING_FIELD");
        cJSON_Delete(json);
        return sms_send_json_response(req, 400, "REJECTED", NULL, "MISSING_FIELD");
    }
    if (!cJSON_IsString(j_text) || !j_text->valuestring[0]) {
        ESP_LOGW(TAG, "[SMS] Champ 'text' manquant ou vide → REJECTED/MISSING_FIELD");
        cJSON_Delete(json);
        return sms_send_json_response(req, 400, "REJECTED", NULL, "MISSING_FIELD");
    }
    if (!cJSON_IsString(j_request_id) || !j_request_id->valuestring[0]) {
        ESP_LOGW(TAG, "[SMS] Champ 'request_id' manquant ou vide → REJECTED/MISSING_FIELD");
        cJSON_Delete(json);
        return sms_send_json_response(req, 400, "REJECTED", NULL, "MISSING_FIELD");
    }

    const char *to         = j_to->valuestring;
    const char *text       = j_text->valuestring;
    const char *request_id = j_request_id->valuestring;
    int ttl_s = SMS_TTL_DEFAULT;
    if (cJSON_IsNumber(j_ttl) && j_ttl->valueint > 0) {
        ttl_s = j_ttl->valueint;
    }
    const char *source = (cJSON_IsString(j_source)) ? j_source->valuestring : "";

    ESP_LOGI(TAG, "[SMS] Champs extraits : to=%s request_id=%s ttl=%d source=%s",
             to, request_id, ttl_s, source);

    // --- Valider le format du numero ---
    if (!sms_validate_number_format(to)) {
        ESP_LOGW(TAG, "[SMS] Format numero invalide : %s → REJECTED/INVALID_NUMBER", to);
        cJSON_Delete(json);
        return sms_send_json_response(req, 422, "REJECTED", request_id, "INVALID_NUMBER");
    }

    // --- Verifier la whitelist ---
    if (!sms_check_whitelist(to)) {
        ESP_LOGW(TAG, "[SMS] Numero hors whitelist : %s → REJECTED/INVALID_NUMBER", to);
        cJSON_Delete(json);
        return sms_send_json_response(req, 422, "REJECTED", request_id, "INVALID_NUMBER");
    }

    // --- Verifier la longueur du texte ---
    if (strlen(text) > SMS_TEXT_MAX_LEN) {
        ESP_LOGW(TAG, "[SMS] Texte trop long : %d chars (max %d) → REJECTED/TEXT_TOO_LONG",
                 (int)strlen(text), SMS_TEXT_MAX_LEN);
        cJSON_Delete(json);
        return sms_send_json_response(req, 422, "REJECTED", request_id, "TEXT_TOO_LONG");
    }

    ESP_LOGI(TAG, "[SMS] Validation OK — mise en queue...");

    // --- Prendre le mutex pour ecrire dans la table ---
    if (xSemaphoreTake(sms_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "[SMS] Timeout mutex table → REJECTED/QUEUE_FULL");
        cJSON_Delete(json);
        return sms_send_json_response(req, 422, "REJECTED", request_id, "QUEUE_FULL");
    }

    // --- Idempotence : si request_id existe deja, renvoyer le statut courant sans re-enqueue ---
    int existing_idx = sms_table_find(request_id);
    if (existing_idx >= 0) {
        const char *existing_status = sms_status_to_str(sms_table[existing_idx].status);
        xSemaphoreGive(sms_mutex);
        ESP_LOGI(TAG, "[SMS] request_id=%s deja connu (status=%s) → reponse idempotente",
                 request_id, existing_status);
        cJSON_Delete(json);
        return sms_send_json_response(req, 200, existing_status, request_id, NULL);
    }

    // --- Ajouter dans la table ---
    int idx = sms_table_add(request_id, to, text, source, ttl_s);

    if (idx < 0) {
        // Table pleine (tous les slots sont non terminaux)
        xSemaphoreGive(sms_mutex);
        ESP_LOGW(TAG, "[SMS] Table pleine (slot non terminal) → REJECTED/QUEUE_FULL");
        cJSON_Delete(json);
        return sms_send_json_response(req, 422, "REJECTED", request_id, "QUEUE_FULL");
    }

    xSemaphoreGive(sms_mutex);

    // --- Envoyer dans la queue FreeRTOS ---
    if (xQueueSend(sms_queue, &idx, 0) != pdTRUE) {
        // La queue est pleine — marquer le slot comme FAILED
        ESP_LOGE(TAG, "[SMS] Queue FreeRTOS pleine ! Slot %d → REJECTED/QUEUE_FULL", idx);

        xSemaphoreTake(sms_mutex, portMAX_DELAY);
        sms_table[idx].status = SMS_STATUS_FAILED;
        strlcpy(sms_table[idx].error_code, "QUEUE_FULL", sizeof(sms_table[idx].error_code));
        strlcpy(sms_table[idx].error_detail, "File d'attente SMS pleine",
                sizeof(sms_table[idx].error_detail));
        xSemaphoreGive(sms_mutex);

        cJSON_Delete(json);
        return sms_send_json_response(req, 422, "REJECTED", request_id, "QUEUE_FULL");
    }

    ESP_LOGI(TAG, "[SMS] ✓ Slot %d en queue — request_id=%s → ACCEPTED", idx, request_id);

    cJSON_Delete(json);
    return sms_send_json_response(req, 200, "ACCEPTED", request_id, NULL);
}

// --- Handler GET /sms/<request_id> ---
static esp_err_t sms_get_handler(httpd_req_t *req)
{
    // Extraire le request_id depuis l'URI : "/sms/sms_001" → "sms_001"
    const char *uri = req->uri;
    ESP_LOGI(TAG, "[SMS] ── GET %s recu ──", uri);

    // Verifier que l'URI commence par "/sms/" et a quelque chose apres
    if (strncmp(uri, "/sms/", 5) != 0 || uri[5] == '\0') {
        ESP_LOGW(TAG, "[SMS] URI invalide : %s → 400", uri);
        return sms_send_json_response(req, 400, "REJECTED", NULL, "MISSING_FIELD");
    }

    const char *request_id = uri + 5;  // Sauter "/sms/"
    ESP_LOGI(TAG, "[SMS] Recherche request_id=%s dans la table...", request_id);

    // --- Chercher dans la table (sous mutex) ---
    xSemaphoreTake(sms_mutex, portMAX_DELAY);

    int idx = sms_table_find(request_id);

    if (idx < 0) {
        xSemaphoreGive(sms_mutex);
        ESP_LOGW(TAG, "[SMS] request_id=%s introuvable → 404", request_id);
        return sms_send_json_response(req, 404, "NOT_FOUND", request_id, NULL);
    }

    // Copier les donnees sous mutex
    sms_entry_t entry;
    memcpy(&entry, &sms_table[idx], sizeof(sms_entry_t));

    xSemaphoreGive(sms_mutex);

    // --- Construire la reponse JSON ---
    ESP_LOGI(TAG, "[SMS] Slot %d trouve : request_id=%s status=%s",
             idx, entry.request_id, sms_status_to_str(entry.status));

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "request_id", entry.request_id);
    cJSON_AddStringToObject(resp, "status", sms_status_to_str(entry.status));

    if (entry.status == SMS_STATUS_FAILED) {
        cJSON_AddStringToObject(resp, "error_code",
                                entry.error_code[0] ? entry.error_code : "UNKNOWN");
        cJSON_AddStringToObject(resp, "error_detail",
                                entry.error_detail[0] ? entry.error_detail : "");
    } else {
        cJSON_AddNullToObject(resp, "error_code");
        cJSON_AddNullToObject(resp, "error_detail");
    }

    const char *json_str = cJSON_PrintUnformatted(resp);

    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t err = httpd_resp_sendstr(req, json_str ? json_str : "{}");

    if (json_str) free((void *)json_str);
    cJSON_Delete(resp);

    ESP_LOGI(TAG, "[SMS] Reponse GET envoyee pour request_id=%s", request_id);
    return err;
}

// --- Handler OPTIONS /sms (preflight CORS) ---
static esp_err_t sms_options_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "[SMS] OPTIONS %s (preflight CORS)", req->uri);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, GET, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

#endif  // ── FIN BLOC HTTP SMS COMMENTÉ ──

// =============================================================================
// 8c. Tache SMS — Envoi reel via bascule PPP/COMMAND (etape 2c)
//
// Cette tache est le coeur du service SMS. Elle consomme la queue FreeRTOS
// et effectue pour chaque message :
//   1. Verification TTL (abandon si expire)
//   2. Attente PPP actif (boucle avec re-verification TTL)
//   3. Verification rate limit (seul un SENT reussi declenche le cooldown)
//   4. Bascule PPP → COMMAND MODE (pause internet temporaire)
//   5. Envoi SMS via API esp_modem (esp_modem_send_sms — pas d'acces UART direct)
//   6. Retour COMMAND → DATA MODE (reprise internet — TOUJOURS, meme si erreur)
//   7. Mise a jour du statut dans la table
//
// Les etapes 4-5-6 sont protegees par modem_mode_mutex pour eviter les conflits
// avec la tache de reconnexion PPP.
// =============================================================================

static void sms_task(void *arg)
{
    ESP_LOGI(TAG, "[SMS-TASK] Tache SMS demarree — en attente de messages...");
    ESP_LOGI(TAG, "[SMS-TASK] Rate limit : %d ms (%d s) entre deux envois reussis",
             SMS_RATE_LIMIT_MS, SMS_RATE_LIMIT_MS / 1000);

    int idx;

    while (true) {
        // =====================================================================
        // Attente bloquante d'un message dans la queue
        // =====================================================================
        if (xQueueReceive(sms_queue, &idx, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        // Verification defensive : idx doit etre dans les bornes de sms_table
        if (idx < 0 || idx >= SMS_TABLE_SIZE) {
            ESP_LOGE(TAG, "[SMS-TASK] ❌ Index invalide recu de la queue : %d", idx);
            continue;
        }

        ESP_LOGI(TAG, "[SMS-TASK] ════════════════════════════════════════");
        ESP_LOGI(TAG, "[SMS-TASK] Message recu de la queue — slot %d", idx);

        sms_processing = true;

        // =====================================================================
        // Lire les donnees du slot (sous mutex)
        // =====================================================================
        xSemaphoreTake(sms_mutex, portMAX_DELAY);

        if (!sms_table[idx].used) {
            xSemaphoreGive(sms_mutex);
            ESP_LOGW(TAG, "[SMS-TASK] Slot %d vide (ecrase?) — ignore", idx);
            sms_processing = false;
            continue;
        }

        // Copier les infos necessaires pour travailler hors mutex
        char request_id[33];
        char to[21];
        char text[161];
        int ttl_s = sms_table[idx].ttl_s;
        int64_t created_ms = sms_table[idx].created_ms;
        strlcpy(request_id, sms_table[idx].request_id, sizeof(request_id));
        strlcpy(to, sms_table[idx].to, sizeof(to));
        strlcpy(text, sms_table[idx].text, sizeof(text));

        xSemaphoreGive(sms_mutex);

        ESP_LOGI(TAG, "[SMS-TASK] request_id=%s to=%s ttl=%ds",
                 request_id, to, ttl_s);
        ESP_LOGI(TAG, "[SMS-TASK] text=\"%.40s%s\"",
                 text, strlen(text) > 40 ? "..." : "");

        // =====================================================================
        // Etape 1 : Verification TTL initiale
        // =====================================================================
        int64_t now_ms = esp_timer_get_time() / 1000;
        int64_t age_ms = now_ms - created_ms;
        int64_t ttl_ms = (int64_t)ttl_s * 1000;

        ESP_LOGI(TAG, "[SMS-TASK] [1/6 TTL] Age du message : %lld ms (TTL : %lld ms)",
                 (long long)age_ms, (long long)ttl_ms);

        if (age_ms > ttl_ms) {
            xSemaphoreTake(sms_mutex, portMAX_DELAY);
            sms_table[idx].status = SMS_STATUS_EXPIRED;
            xSemaphoreGive(sms_mutex);
            ESP_LOGW(TAG, "[SMS-TASK] [1/6 TTL] ⏰ EXPIRE ! age=%llds > ttl=%ds → EXPIRED",
                     (long long)(age_ms / 1000), ttl_s);
            ESP_LOGI(TAG, "[SMS-TASK] ════════════════════════════════════════");
            sms_processing = false;
            continue;
        }

        // =====================================================================
        // Etape 2 : Attente PPP actif (boucle avec re-verification TTL)
        // On ne tente jamais d'envoyer un SMS si PPP n'est pas connecte.
        // On boucle toutes les SMS_PPP_CHECK_MS en revérifiant le TTL.
        // =====================================================================
        ESP_LOGI(TAG, "[SMS-TASK] [2/6 PPP] Verification connexion PPP...");

        bool ppp_ok = false;
        while (!ppp_ok) {
            EventBits_t bits = xEventGroupGetBits(modem_event_group);
            if (bits & PPP_CONNECTED_BIT) {
                ppp_ok = true;
                ESP_LOGI(TAG, "[SMS-TASK] [2/6 PPP] PPP actif ✓");
            } else {
                // PPP inactif — verifier que le TTL n'a pas expire pendant l'attente
                now_ms = esp_timer_get_time() / 1000;
                age_ms = now_ms - created_ms;

                if (age_ms > ttl_ms) {
                    // TTL expire pendant l'attente PPP
                    xSemaphoreTake(sms_mutex, portMAX_DELAY);
                    sms_table[idx].status = SMS_STATUS_EXPIRED;
                    xSemaphoreGive(sms_mutex);
                    ESP_LOGW(TAG, "[SMS-TASK] [2/6 PPP] ⏰ TTL expire pendant attente PPP ! "
                             "age=%llds > ttl=%ds → EXPIRED",
                             (long long)(age_ms / 1000), ttl_s);
                    break;  // Sortir de la boucle while PPP
                }

                ESP_LOGW(TAG, "[SMS-TASK] [2/6 PPP] PPP inactif — attente %d ms "
                         "(age=%llds, ttl=%ds)...",
                         SMS_PPP_CHECK_MS, (long long)(age_ms / 1000), ttl_s);
                vTaskDelay(pdMS_TO_TICKS(SMS_PPP_CHECK_MS));
            }
        }

        // Si on est sorti de la boucle sans PPP (TTL expire), passer au suivant
        if (!ppp_ok) {
            ESP_LOGI(TAG, "[SMS-TASK] ════════════════════════════════════════");
            sms_processing = false;
            continue;
        }

        // =====================================================================
        // Etape 3 : Verification rate limit
        // Rate limit mis a jour sur toute tentative (reussie ou pas).
        // Seul un envoi reussi declenche l'envoi d'un ACK vers la Waveshare.
        // =====================================================================
        ESP_LOGI(TAG, "[SMS-TASK] [3/6 RATE] Verification rate limit...");

        xSemaphoreTake(sms_mutex, portMAX_DELAY);
        int64_t last_sent = last_sms_sent_ms;
        xSemaphoreGive(sms_mutex);

        now_ms = esp_timer_get_time() / 1000;

        if (last_sent > 0) {
            int64_t elapsed_ms = now_ms - last_sent;
            int64_t remaining_ms = SMS_RATE_LIMIT_MS - elapsed_ms;

            if (remaining_ms > 0) {
                // Rate limit actif — marquer FAILED
                ESP_LOGW(TAG, "[SMS-TASK] [3/6 RATE] ⛔ Rate limit actif ! "
                         "Dernier envoi il y a %llds, reste %llds de cooldown",
                         (long long)(elapsed_ms / 1000), (long long)(remaining_ms / 1000));

                xSemaphoreTake(sms_mutex, portMAX_DELAY);
                sms_table[idx].status = SMS_STATUS_FAILED;
                strlcpy(sms_table[idx].error_code, "RATE_LIMITED",
                        sizeof(sms_table[idx].error_code));
                strlcpy(sms_table[idx].error_detail, "Trop tot apres le dernier envoi",
                        sizeof(sms_table[idx].error_detail));
                xSemaphoreGive(sms_mutex);

                ESP_LOGI(TAG, "[SMS-TASK] ════════════════════════════════════════");
                sms_processing = false;
                continue;
            }
        }

        ESP_LOGI(TAG, "[SMS-TASK] [3/6 RATE] Rate limit OK ✓ (dernier envoi : %s)",
                 last_sent > 0 ? "il y a plus de 5 min" : "aucun");

        // =====================================================================
        // Etapes 4-5-6 : Bascule modem + envoi AT + retour DATA
        //
        // PROTEGEES PAR modem_mode_mutex
        // Ce mutex empeche la tache de reconnexion PPP de tenter une bascule
        // COMMAND/DATA pendant qu'on envoie un SMS (et vice versa).
        // =====================================================================
        ESP_LOGI(TAG, "[SMS-TASK] [4/6 CMD] Acquisition mutex modem...");

        if (xSemaphoreTake(modem_mode_mutex, pdMS_TO_TICKS(30000)) != pdTRUE) {
            // Timeout 30s — la tache reconnexion tient probablement le mutex
            ESP_LOGE(TAG, "[SMS-TASK] [4/6 CMD] Timeout mutex modem (30s) → FAILED");

            xSemaphoreTake(sms_mutex, portMAX_DELAY);
            sms_table[idx].status = SMS_STATUS_FAILED;
            strlcpy(sms_table[idx].error_code, "MODEM_BUSY",
                    sizeof(sms_table[idx].error_code));
            strlcpy(sms_table[idx].error_detail, "Modem occupe (reconnexion en cours?)",
                    sizeof(sms_table[idx].error_detail));
            xSemaphoreGive(sms_mutex);

            ESP_LOGI(TAG, "[SMS-TASK] ════════════════════════════════════════");
            sms_processing = false;
            continue;
        }

        ESP_LOGI(TAG, "[SMS-TASK] [4/6 CMD] Mutex modem acquis ✓");

        // --- Etape 4 : Passer en SENDING + Bascule PPP → COMMAND MODE ---
        xSemaphoreTake(sms_mutex, portMAX_DELAY);
        sms_table[idx].status = SMS_STATUS_SENDING;
        xSemaphoreGive(sms_mutex);

        ESP_LOGI(TAG, "[SMS-TASK] [4/6 CMD] Status → SENDING");
        ESP_LOGI(TAG, "[SMS-TASK] [4/6 CMD] Bascule PPP → COMMAND MODE (pause internet)...");

        esp_err_t err = esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "[SMS-TASK] [4/6 CMD] ❌ ECHEC bascule COMMAND MODE : %s",
                     esp_err_to_name(err));

            xSemaphoreTake(sms_mutex, portMAX_DELAY);
            sms_table[idx].status = SMS_STATUS_FAILED;
            strlcpy(sms_table[idx].error_code, "COMMAND_MODE_FAIL",
                    sizeof(sms_table[idx].error_code));
            strlcpy(sms_table[idx].error_detail, esp_err_to_name(err),
                    sizeof(sms_table[idx].error_detail));
            xSemaphoreGive(sms_mutex);

            // Tenter de revenir en DATA MODE quand meme (securite)
            ESP_LOGW(TAG, "[SMS-TASK] [4/6 CMD] Tentative retour DATA MODE (securite)...");
            esp_modem_set_mode(dce, ESP_MODEM_MODE_DATA);

            xSemaphoreGive(modem_mode_mutex);  // TOUJOURS liberer le mutex
            ESP_LOGI(TAG, "[SMS-TASK] ════════════════════════════════════════");
            sms_processing = false;
            continue;
        }

        ESP_LOGI(TAG, "[SMS-TASK] [4/6 CMD] COMMAND MODE actif ✓ (internet coupe)");

        // --- Etape 5 : Envoi SMS via API esp_modem ---
        // Utilise l'API publique esp_modem qui gere internement la sequence
        // AT+CMGF=1 → AT+CMGS="num" → texte + Ctrl+Z → attente +CMGS:
        // sans acces UART direct (pas de conflit avec la tache DTE interne).
        ESP_LOGI(TAG, "[SMS-TASK] [5/6 SEND] 📱 Envoi SMS a %s...", to);
        ESP_LOGI(TAG, "[SMS-TASK] [5/6 SEND] Texte : \"%s\"", text);

        bool send_success = false;
        char at_error_code[33] = {0};
        char at_error_detail[65] = {0};

        // 5a. Mode texte SMS (AT+CMGF=1)
        esp_err_t sms_err = esp_modem_sms_txt_mode(dce, true);
        if (sms_err != ESP_OK) {
            ESP_LOGE(TAG, "[SMS-TASK] [5/6 SEND] ❌ esp_modem_sms_txt_mode echec : %s",
                     esp_err_to_name(sms_err));
            strlcpy(at_error_code, "CMGF_FAILED", sizeof(at_error_code));
            strlcpy(at_error_detail, esp_err_to_name(sms_err), sizeof(at_error_detail));
        } else {
            ESP_LOGI(TAG, "[SMS-TASK] [5/6 SEND] Mode texte OK ✓");

            // 5b. Jeu de caracteres GSM (AT+CSCS="GSM")
            sms_err = esp_modem_sms_character_set(dce);
            if (sms_err != ESP_OK) {
                // Non bloquant : on log un warning mais on continue l'envoi
                ESP_LOGW(TAG, "[SMS-TASK] [5/6 SEND] esp_modem_sms_character_set echec : %s "
                         "(non bloquant, on continue)", esp_err_to_name(sms_err));
            } else {
                ESP_LOGI(TAG, "[SMS-TASK] [5/6 SEND] Charset GSM OK ✓");
            }

            // 5c. Envoi effectif (AT+CMGS + texte + Ctrl+Z + attente +CMGS:)
            ESP_LOGI(TAG, "[SMS-TASK] [5/6 SEND] esp_modem_send_sms en cours "
                     "(peut prendre jusqu'a 120s sur Cat-M)...");
            sms_err = esp_modem_send_sms(dce, to, text);
            if (sms_err == ESP_OK) {
                send_success = true;
                ESP_LOGI(TAG, "[SMS-TASK] [5/6 SEND] ✅ esp_modem_send_sms OK");
            } else {
                ESP_LOGE(TAG, "[SMS-TASK] [5/6 SEND] ❌ esp_modem_send_sms echec : %s",
                         esp_err_to_name(sms_err));
                strlcpy(at_error_code, "SEND_FAILED", sizeof(at_error_code));
                strlcpy(at_error_detail, esp_err_to_name(sms_err), sizeof(at_error_detail));
            }
        }

        // --- Etape 6 : Retour COMMAND → DATA MODE (TOUJOURS) ---
        ESP_LOGI(TAG, "[SMS-TASK] [6/6 DATA] Retour DATA MODE (reprise internet)...");

        esp_err_t data_err = esp_modem_set_mode(dce, ESP_MODEM_MODE_DATA);

        if (data_err == ESP_OK) {
            ESP_LOGI(TAG, "[SMS-TASK] [6/6 DATA] DATA MODE restaure ✓ (internet reprend)");
        } else {
            ESP_LOGE(TAG, "[SMS-TASK] [6/6 DATA] ❌ ECHEC retour DATA MODE : %s",
                     esp_err_to_name(data_err));
            ESP_LOGE(TAG, "[SMS-TASK] [6/6 DATA] ⚠️ INTERNET PEUT ETRE COUPE ! "
                     "La tache reconnexion PPP prendra le relais.");
        }

        // TOUJOURS liberer le mutex modem, quel que soit le resultat
        xSemaphoreGive(modem_mode_mutex);
        ESP_LOGI(TAG, "[SMS-TASK] Mutex modem libere");

        // =====================================================================
        // Mise a jour finale du statut dans la table
        // =====================================================================
        xSemaphoreTake(sms_mutex, portMAX_DELAY);

        // Rate limit : mis a jour sur TOUTE tentative (reussie ou pas)
        last_sms_sent_ms = esp_timer_get_time() / 1000;

        if (send_success) {
            sms_table[idx].status = SMS_STATUS_SENT;

            // Envoyer ACK si le SMS vient de la Waveshare (UDP)
            if (sms_table[idx].from_udp) {
                send_udp_ack();
                sms_table[idx].from_udp = false;
                ESP_LOGI(TAG, "[SMS-TASK] ACK envoyé à la Waveshare");
            }

            ESP_LOGI(TAG, "[SMS-TASK] ✅ SENT — request_id=%s to=%s", request_id, to);
            ESP_LOGI(TAG, "[SMS-TASK] Rate limit : prochain envoi possible dans %ds",
                     SMS_RATE_LIMIT_MS / 1000);
        } else {
            sms_table[idx].status = SMS_STATUS_FAILED;
            strlcpy(sms_table[idx].error_code,
                    at_error_code[0] ? at_error_code : "SEND_FAILED",
                    sizeof(sms_table[idx].error_code));
            strlcpy(sms_table[idx].error_detail,
                    at_error_detail[0] ? at_error_detail : "Erreur inconnue",
                    sizeof(sms_table[idx].error_detail));

            ESP_LOGE(TAG, "[SMS-TASK] ❌ FAILED — request_id=%s code=%s detail=%s",
                     request_id, sms_table[idx].error_code, sms_table[idx].error_detail);
        }

        xSemaphoreGive(sms_mutex);

        sms_processing = false;
        ESP_LOGI(TAG, "[SMS-TASK] ════════════════════════════════════════");
    }
}

// =============================================================================
// 8d. Tache reconnexion PPP
//
// Dort tant que PPP est actif. Se reveille sur PPP_RECONNECT_BIT (positionne
// par ip_event_handler quand PPP tombe, ou par app_main si PPP timeout au boot).
//
// Tente de reconnecter indefiniment avec un backoff plafonne a 120s.
// Ne reboote JAMAIS sur echec de reconnexion seul.
//
// Watchdog : si pas de heartbeat (Waveshare) depuis 2h ET PPP toujours down
// → reboot. C'est la seule condition de reboot du systeme.
//
// Utilise modem_mode_mutex pour eviter les conflits avec la tache SMS.
// =============================================================================

static void ppp_reconnect_task(void *arg)
{
    ESP_LOGI(TAG, "[PPP-RECONNECT] Tache demarree — en veille");

    if (!dce) {
        ESP_LOGE(TAG, "[PPP-RECONNECT] DCE non initialise — tache inactive");
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        // =====================================================================
        // Dormir jusqu'a ce que PPP tombe
        // =====================================================================
        xEventGroupWaitBits(modem_event_group, PPP_RECONNECT_BIT,
                            pdTRUE,     // Clear le bit au reveil
                            pdFALSE,
                            portMAX_DELAY);

        ESP_LOGW(TAG, "[PPP-RECONNECT] ════════════════════════════════════");
        ESP_LOGW(TAG, "[PPP-RECONNECT] PPP perdu — debut reconnexion (infinie, backoff plafonne)");

        bool reconnected = false;
        int attempt = 0;

        while (!reconnected) {
            // Backoff : suit la table tant qu'il y a des paliers, puis plafonne au dernier
            int backoff_idx = (attempt < PPP_RECONNECT_MAX) ? attempt : PPP_RECONNECT_MAX - 1;
            int backoff_ms = ppp_reconnect_backoff_ms[backoff_idx];

            ESP_LOGI(TAG, "[PPP-RECONNECT] Attente backoff %d ms (essai %d)...",
                     backoff_ms, attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));

            // Note : le watchdog (reboot si PPP down depuis 2h) est desormais
            // gere par udp_bridge_task, independamment de cette tache. Cela
            // garantit le reboot meme si ppp_reconnect_task est endormie ou
            // bloquee. Voir bloc "Surveillance PPP" dans udp_bridge_task.

            // Verifier si PPP n'est pas revenu tout seul
            EventBits_t bits = xEventGroupGetBits(modem_event_group);
            if (bits & PPP_CONNECTED_BIT) {
                ESP_LOGI(TAG, "[PPP-RECONNECT] ✅ PPP revenu tout seul (avant essai %d)",
                         attempt + 1);
                reconnected = true;
                break;
            }

            // Prendre le mutex modem (empeche la tache SMS de basculer en meme temps)
            ESP_LOGI(TAG, "[PPP-RECONNECT] Essai %d : acquisition mutex modem...",
                     attempt + 1);

            if (xSemaphoreTake(modem_mode_mutex, pdMS_TO_TICKS(60000)) != pdTRUE) {
                ESP_LOGW(TAG, "[PPP-RECONNECT] Timeout mutex modem (60s) — "
                         "la tache SMS tient le mutex, on reessaie");
                attempt++;
                continue;
            }

            ESP_LOGI(TAG, "[PPP-RECONNECT] Mutex modem acquis ✓");
            ESP_LOGI(TAG, "[PPP-RECONNECT] Bascule COMMAND → DATA...");

            // Basculer en COMMAND (arrete PPP proprement)
            esp_err_t err = esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "[PPP-RECONNECT] Echec passage COMMAND : %s",
                         esp_err_to_name(err));
                xSemaphoreGive(modem_mode_mutex);
                attempt++;
                continue;
            }

            vTaskDelay(pdMS_TO_TICKS(2000));  // Laisser le modem se stabiliser

            // Repasser en DATA (relance negociation PPP)
            err = esp_modem_set_mode(dce, ESP_MODEM_MODE_DATA);

            // Liberer le mutex AVANT l'attente IP (qui peut etre longue)
            xSemaphoreGive(modem_mode_mutex);
            ESP_LOGI(TAG, "[PPP-RECONNECT] Mutex modem libere");

            if (err != ESP_OK) {
                ESP_LOGE(TAG, "[PPP-RECONNECT] Echec passage DATA : %s",
                         esp_err_to_name(err));
                attempt++;
                continue;
            }

            // Attendre que PPP obtienne une IP
            ESP_LOGI(TAG, "[PPP-RECONNECT] DATA MODE ok — attente IP (timeout %ds)...",
                     PPP_TIMEOUT_MS / 1000);

            bits = xEventGroupWaitBits(
                modem_event_group, PPP_CONNECTED_BIT,
                pdFALSE, pdFALSE,
                pdMS_TO_TICKS(PPP_TIMEOUT_MS));

            if (bits & PPP_CONNECTED_BIT) {
                ESP_LOGI(TAG, "[PPP-RECONNECT] ✅ PPP reconnecte (essai %d)",
                         attempt + 1);
                reconnected = true;
                break;
            }

            ESP_LOGW(TAG, "[PPP-RECONNECT] Timeout IP (essai %d)", attempt + 1);
            attempt++;
        }

        ESP_LOGI(TAG, "[PPP-RECONNECT] Reconnexion reussie — reconfiguration NAT/DNS...");

        // Reconfigurer NAT+DNS apres reconnexion PPP (nouveau DNS possible).
        // Appele ici (dans une tache) et non dans ip_event_handler,
        // pour eviter les problemes de thread-context avec lwIP.
        if (ap_netif) {
            enable_napt();
        }

        ESP_LOGI(TAG, "[PPP-RECONNECT] Retour en veille");
        ESP_LOGI(TAG, "[PPP-RECONNECT] ════════════════════════════════════");
    }
}

// =============================================================================
// SECTION HTTP HEARTBEAT + SERVEUR — MISE EN COMMENTAIRE (v4.3 — remplacé par UDP)
// =============================================================================
#if 0  // ── DEBUT BLOC HTTP HEARTBEAT + SERVEUR COMMENTÉ ──

// --- Handler GET /heartbeat ---
// Appele par le Waveshare apres chaque publication MQTT reussie.
// Met a jour le timestamp d'activite pour le watchdog.
static esp_err_t heartbeat_get_handler(httpd_req_t *req)
{
    portENTER_CRITICAL(&activity_spinlock);
    last_activity_ms = esp_timer_get_time() / 1000;
    portEXIT_CRITICAL(&activity_spinlock);

    ESP_LOGI(TAG, "[HEARTBEAT] Signe de vie recu du Waveshare");

    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, "{\"status\":\"OK\"}");
}

// =============================================================================
// 9. Demarrage du serveur HTTP
// =============================================================================

static httpd_handle_t http_server_init(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    // Activer le matching wildcard pour supporter GET /sms/<id>
    config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server = NULL;

    ESP_LOGI(TAG, "[HTTP] Demarrage serveur HTTP (port %d)...", config.server_port);

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "[HTTP] ECHEC demarrage serveur !");
        return NULL;
    }

    // --- POST /sms ---
    httpd_uri_t sms_post_uri = {};
    sms_post_uri.uri     = "/sms";
    sms_post_uri.method  = HTTP_POST;
    sms_post_uri.handler = sms_post_handler;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &sms_post_uri));
    ESP_LOGI(TAG, "[HTTP] Endpoint enregistre : POST /sms");

    // --- GET /sms/* (consultation statut) ---
    httpd_uri_t sms_get_uri = {};
    sms_get_uri.uri     = "/sms/*";
    sms_get_uri.method  = HTTP_GET;
    sms_get_uri.handler = sms_get_handler;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &sms_get_uri));
    ESP_LOGI(TAG, "[HTTP] Endpoint enregistre : GET /sms/<id>");

    // --- OPTIONS /sms et /sms/* (preflight CORS) ---
    httpd_uri_t sms_options_uri = {};
    sms_options_uri.uri     = "/sms";
    sms_options_uri.method  = HTTP_OPTIONS;
    sms_options_uri.handler = sms_options_handler;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &sms_options_uri));

    httpd_uri_t sms_options_wildcard_uri = {};
    sms_options_wildcard_uri.uri     = "/sms/*";
    sms_options_wildcard_uri.method  = HTTP_OPTIONS;
    sms_options_wildcard_uri.handler = sms_options_handler;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &sms_options_wildcard_uri));
    ESP_LOGI(TAG, "[HTTP] Endpoint enregistre : OPTIONS /sms + /sms/* (CORS)");

    // --- GET /heartbeat (watchdog — signe de vie du Waveshare) ---
    httpd_uri_t heartbeat_get_uri = {};
    heartbeat_get_uri.uri     = "/heartbeat";
    heartbeat_get_uri.method  = HTTP_GET;
    heartbeat_get_uri.handler = heartbeat_get_handler;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &heartbeat_get_uri));
    ESP_LOGI(TAG, "[HTTP] Endpoint enregistre : GET /heartbeat (watchdog)");

    ESP_LOGI(TAG, "[HTTP] Serveur HTTP operationnel");
    return server;
}

#endif  // ── FIN BLOC HTTP HEARTBEAT + SERVEUR COMMENTÉ ──

// =============================================================================
// Point d'entree principal
// =============================================================================

extern "C" void app_main(void)
{
    // Attente reconnexion USB-CDC : laisse le temps au moniteur serie
    // de se reconnecter apres un reset/flash (enumeration USB ~2-3s)
    vTaskDelay(pdMS_TO_TICKS(15000));

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Routeur WiFi/GSM — Serre Connectee");
    ESP_LOGI(TAG, "  Firmware v9.0-udp-bridge");
    ESP_LOGI(TAG, "========================================");

    // =========================================================================
    // Etape 0 : Infrastructure ESP-IDF
    // =========================================================================

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS corrompu, reinitialisation...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "[INIT] NVS initialise");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_LOGI(TAG, "[INIT] esp_netif initialise");

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_LOGI(TAG, "[INIT] Boucle d'evenements creee");

    modem_event_group = xEventGroupCreate();

    // Initialiser le watchdog : 2h de grace a partir du boot
    portENTER_CRITICAL(&activity_spinlock);
    last_activity_ms = esp_timer_get_time() / 1000;
    portEXIT_CRITICAL(&activity_spinlock);

    // =========================================================================
    // Etape 1 : PMU AXP2101 (alimentation modem)
    // =========================================================================
    ESP_LOGI(TAG, "--- Etape 1/13 : PMU AXP2101 ---");
    if (pmu_init() != ESP_OK) {
        ESP_LOGE(TAG, "ERREUR FATALE : PMU non initialisee !");
        ESP_LOGE(TAG, "Le modem ne peut pas etre alimente. Arret.");
        return;
    }

    // =========================================================================
    // Etape 2 : GPIO modem
    // =========================================================================
    ESP_LOGI(TAG, "--- Etape 2/13 : GPIO modem ---");
    modem_gpio_init();

    // =========================================================================
    // Etape 3 : PPPoS (connexion cellulaire)
    // =========================================================================
    ESP_LOGI(TAG, "--- Etape 3/13 : PPPoS (connexion cellulaire) ---");
    dce = modem_init_ppp();
    if (!dce) {
        ESP_LOGE(TAG, "ERREUR : Modem non initialise apres %d essais — reboot",
                 AT_RETRY_MAX);
        vTaskDelay(pdMS_TO_TICKS(2000));  // Laisser les logs sortir
        esp_restart();
    }

    // =========================================================================
    // Etape 4 : Attente connexion PPP
    // =========================================================================
    ESP_LOGI(TAG, "--- Etape 4/13 : Attente connexion PPP (timeout %ds) ---",
             PPP_TIMEOUT_MS / 1000);

    EventBits_t bits = xEventGroupWaitBits(
        modem_event_group,
        PPP_CONNECTED_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(PPP_TIMEOUT_MS));

    if (!(bits & PPP_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "========================================");
        ESP_LOGW(TAG, "  PPP : pas de connexion apres %d secondes", PPP_TIMEOUT_MS / 1000);
        ESP_LOGW(TAG, "  Demarrage en MODE DEGRADE (pas d'internet)");
        ESP_LOGW(TAG, "  La tache reconnexion PPP prendra le relais");
        ESP_LOGW(TAG, "========================================");
        // Signaler a la tache reconnexion (creee plus tard) de travailler des son demarrage
        xEventGroupSetBits(modem_event_group, PPP_RECONNECT_BIT);
    } else {
        ESP_LOGI(TAG, "[PPP] Connexion cellulaire etablie !");
    }

    // =========================================================================
    // Etape 5 : WiFi SoftAP (demarre APRES le modem)
    // =========================================================================
    ESP_LOGI(TAG, "--- Etape 5/13 : WiFi SoftAP ---");
    wifi_init_softap();

    // =========================================================================
    // Etape 6 : NAPT (DNS + NAT L3)
    // =========================================================================
    ESP_LOGI(TAG, "--- Etape 6/13 : NAPT ---");
    enable_napt();

    // =========================================================================
    // Etape 7 : Mutex modem (AVANT les taches SMS et reconnexion)
    // =========================================================================
    ESP_LOGI(TAG, "--- Etape 7/13 : Mutex modem ---");
    modem_mode_mutex = xSemaphoreCreateMutex();
    if (!modem_mode_mutex) {
        ESP_LOGE(TAG, "ERREUR : Impossible de creer le mutex modem !");
        return;
    }
    ESP_LOGI(TAG, "[MODEM] Mutex mode modem cree");

    // =========================================================================
    // Etape 8 : Queue + Mutex + Table SMS
    // (AVANT le serveur HTTP — les handlers POST/GET en dependent)
    // =========================================================================
    ESP_LOGI(TAG, "--- Etape 8/13 : Queue + Mutex + Table SMS ---");

    sms_mutex = xSemaphoreCreateMutex();
    if (!sms_mutex) {
        ESP_LOGE(TAG, "ERREUR : Impossible de creer le mutex SMS !");
        return;
    }
    ESP_LOGI(TAG, "[SMS] Mutex cree");

    sms_queue = xQueueCreate(SMS_QUEUE_SIZE, sizeof(int));
    if (!sms_queue) {
        ESP_LOGE(TAG, "ERREUR : Impossible de creer la queue SMS !");
        return;
    }
    ESP_LOGI(TAG, "[SMS] Queue creee (taille=%d)", SMS_QUEUE_SIZE);

    // Initialiser la table de statuts
    memset(sms_table, 0, sizeof(sms_table));
    ESP_LOGI(TAG, "[SMS] Table de statuts initialisee (%d slots)", SMS_TABLE_SIZE);

    // =========================================================================
    // Etape 9 : Serveur HTTP — DÉSACTIVÉ (v4.3 — remplacé par UDP)
    // Le code est conservé en commentaire (#if 0) dans les sections 7 et 9.
    // =========================================================================
    ESP_LOGI(TAG, "--- Etape 9/13 : Serveur HTTP --- DESACTIVE (remplace par UDP)");

    // =========================================================================
    // Etape 10 : Tache SMS
    // =========================================================================
    ESP_LOGI(TAG, "--- Etape 10/13 : Tache SMS ---");

    BaseType_t task_created = xTaskCreate(
        sms_task,           // Fonction
        "sms_task",         // Nom
        4096,               // Stack (4 Ko)
        NULL,               // Parametre : plus besoin, dce est global
        5,                  // Priorite (moyenne)
        NULL                // Handle (pas besoin)
    );

    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "ERREUR : Impossible de creer la tache SMS !");
    } else {
        ESP_LOGI(TAG, "[SMS] Tache SMS demarree (stack=4096, prio=5)");
    }

    // =========================================================================
    // Etape 11 : Tache reconnexion PPP
    // =========================================================================
    ESP_LOGI(TAG, "--- Etape 11/13 : Tache reconnexion PPP ---");

    task_created = xTaskCreate(
        ppp_reconnect_task,     // Fonction
        "ppp_reconnect",        // Nom
        3072,                   // Stack (3 Ko — pas de gros buffers locaux)
        NULL,                   // Parametre : dce est global
        4,                      // Priorite (sous la tache SMS)
        NULL                    // Handle (pas besoin)
    );

    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "ERREUR : Impossible de creer la tache reconnexion PPP !");
    } else {
        ESP_LOGI(TAG, "[PPP] Tache reconnexion demarree (stack=3072, prio=4)");
    }

    // =========================================================================
    // Etape 12 : Tache UDP Bridge (communication Waveshare ↔ LilyGo)
    // =========================================================================
    ESP_LOGI(TAG, "--- Etape 12/13 : Tache UDP Bridge ---");

    // Créer le socket UDP
    udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp_sock < 0) {
        ESP_LOGE(TAG, "[UDP] ERREUR : Impossible de creer le socket UDP !");
    } else {
        // Bind sur le port local
        struct sockaddr_in local_addr;
        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin_family = AF_INET;
        local_addr.sin_port = htons(UDP_LOCAL_PORT);
        local_addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(udp_sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
            ESP_LOGE(TAG, "[UDP] ERREUR : Bind port %d echoue !", UDP_LOCAL_PORT);
            close(udp_sock);
            udp_sock = -1;
        } else {
            // Préparer l'adresse Waveshare
            memset(&waveshare_addr, 0, sizeof(waveshare_addr));
            waveshare_addr.sin_family = AF_INET;
            waveshare_addr.sin_port = htons(UDP_REMOTE_PORT);
            inet_aton(WAVESHARE_IP, &waveshare_addr.sin_addr);

            // Créer la tâche
            task_created = xTaskCreate(
                udp_bridge_task,
                "udp_bridge",
                UDP_TASK_STACK,
                NULL,
                UDP_TASK_PRIORITY,
                NULL
            );

            if (task_created != pdPASS) {
                ESP_LOGE(TAG, "[UDP] ERREUR : Impossible de creer la tache UDP Bridge !");
            } else {
                ESP_LOGI(TAG, "[UDP] Tache UDP Bridge demarree (port=%d, stack=%d, prio=%d)",
                         UDP_LOCAL_PORT, UDP_TASK_STACK, UDP_TASK_PRIORITY);
            }
        }
    }

    // =========================================================================
    // Etape 13 : SMS de boot (signe de vie LilyGo)
    // Configurable via BOOT_SMS_ENABLED
    // =========================================================================
    ESP_LOGI(TAG, "--- Etape 13/13 : SMS de boot ---");

    if (BOOT_SMS_ENABLED) {
        // Calcul pourcentage batterie par interpolation linéaire (3.0V=0%, 4.2V=100%)
        int bat_mv = pmu.getBattVoltage();
        int bat_pct = (bat_mv - 3000) * 100 / (4200 - 3000);
        if (bat_pct < 0)   bat_pct = 0;
        if (bat_pct > 100) bat_pct = 100;

        char boot_sms_text[161];
        snprintf(boot_sms_text, sizeof(boot_sms_text),
                 "Reseau de la serre de Marie-Pierre actif (MPFE) - Batterie %d%%",
                 bat_pct);

        ESP_LOGI(TAG, "[SMS-BOOT] Texte : %s", boot_sms_text);
        ESP_LOGI(TAG, "[SMS-BOOT] Batterie : %d mV → %d%%", bat_mv, bat_pct);

        // Envoyer à chaque numéro de la whitelist
        for (size_t i = 0; i < SMS_WHITELIST_SIZE; i++) {
            xSemaphoreTake(sms_mutex, portMAX_DELAY);

            char req_id[33];
            snprintf(req_id, sizeof(req_id), "boot_%d", (int)i);

            int idx = sms_table_add(req_id, sms_whitelist[i], boot_sms_text,
                                    "lilygo_boot", SMS_TTL_DEFAULT);

            if (idx >= 0) {
                sms_table[idx].from_udp = false;  // Pas d'ACK attendu
                xSemaphoreGive(sms_mutex);

                if (xQueueSend(sms_queue, &idx, 0) == pdTRUE) {
                    ESP_LOGI(TAG, "[SMS-BOOT] SMS de boot en queue pour %s (slot %d)",
                             sms_whitelist[i], idx);
                } else {
                    ESP_LOGW(TAG, "[SMS-BOOT] Queue pleine — SMS de boot non envoye pour %s",
                             sms_whitelist[i]);
                }
            } else {
                xSemaphoreGive(sms_mutex);
                ESP_LOGW(TAG, "[SMS-BOOT] Table pleine — SMS de boot non envoye pour %s",
                         sms_whitelist[i]);
            }
        }
    } else {
        ESP_LOGI(TAG, "[SMS-BOOT] SMS de boot desactive (BOOT_SMS_ENABLED=false)");
    }

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  ROUTEUR OPERATIONNEL");
    ESP_LOGI(TAG, "  WiFi AP  : %s (%s)", WIFI_AP_SSID, WIFI_AP_IP);
    ESP_LOGI(TAG, "  NAT L3   : WiFi -> PPPoS -> Cat-M -> Internet");
    ESP_LOGI(TAG, "  UDP Bridge : port %d (ecoute) → %s:%d (Waveshare)",
             UDP_LOCAL_PORT, WAVESHARE_IP, UDP_REMOTE_PORT);
    ESP_LOGI(TAG, "  Queue SMS : %d slots | Table : %d slots", SMS_QUEUE_SIZE, SMS_TABLE_SIZE);
    ESP_LOGI(TAG, "  Rate limit : %ds entre deux SMS", SMS_RATE_LIMIT_MS / 1000);
    ESP_LOGI(TAG, "  STATE : toutes les %ds", STATE_INTERVAL_MS / 1000);
    ESP_LOGI(TAG, "  PPP reconnexion : infinie avec backoff plafonne a 120s");
    ESP_LOGI(TAG, "  SMS de boot : %s", BOOT_SMS_ENABLED ? "ACTIF" : "DESACTIVE");
    ESP_LOGI(TAG, "  Alerte SMS Waveshare muette : %s (delai %ds)",
             ALERTE_ABSENCE_HEARTBEAT_SMS_ENABLED ? "ACTIF" : "DESACTIVE",
             ALERTE_ABSENCE_HEARTBEAT_DELAI_MS / 1000);
    ESP_LOGI(TAG, "  Surveillance PPP : relance toutes les %lus, reboot apres %lus",
             PPP_DOWN_RELAUNCH_MS / 1000,
             PPP_DOWN_REBOOT_MS / 1000);
    ESP_LOGI(TAG, "  Test ping Internet : toutes les %lus (cible primaire %s, +2 fallbacks)",
             PING_CHECK_INTERVAL_MS / 1000,
             PING_TARGET_PRIMARY);
    ESP_LOGI(TAG, "  Reboot watchdog PPP : %s",
             WATCHDOG_REBOOT_ENABLED ? "ACTIF" : "DESACTIVE");
    ESP_LOGI(TAG, "  Alerte batterie : %s (seuil bas %d%%, retabli %d%%)",
             ALERTE_BATTERIE_ENABLED ? "ACTIF" : "DESACTIVE",
             ALERTE_BATTERIE_SEUIL_BAS_POURCENT,
             ALERTE_BATTERIE_SEUIL_HAUT_POURCENT);
    ESP_LOGI(TAG, "  Waveshare: IP statique %s attendue", WAVESHARE_IP);
    ESP_LOGI(TAG, "========================================");

    // =========================================================================
    // DIAGNOSTIC RESEAU — Etat complet apres initialisation
    // =========================================================================
    ESP_LOGI(TAG, "[DIAG-NET] ═══ DUMP RESEAU POST-INIT ═══");

    // PPP
    esp_netif_ip_info_t ppp_ip_info;
    if (esp_netif_get_ip_info(ppp_netif, &ppp_ip_info) == ESP_OK) {
        ESP_LOGI(TAG, "[DIAG-NET] PPP  : IP=" IPSTR " GW=" IPSTR " Mask=" IPSTR,
                 IP2STR(&ppp_ip_info.ip), IP2STR(&ppp_ip_info.gw),
                 IP2STR(&ppp_ip_info.netmask));
    } else {
        ESP_LOGW(TAG, "[DIAG-NET] PPP  : pas d'IP");
    }

    // AP
    esp_netif_ip_info_t ap_ip_info;
    if (esp_netif_get_ip_info(ap_netif, &ap_ip_info) == ESP_OK) {
        ESP_LOGI(TAG, "[DIAG-NET] AP   : IP=" IPSTR " GW=" IPSTR " Mask=" IPSTR,
                 IP2STR(&ap_ip_info.ip), IP2STR(&ap_ip_info.gw),
                 IP2STR(&ap_ip_info.netmask));
    } else {
        ESP_LOGW(TAG, "[DIAG-NET] AP   : pas d'IP");
    }

    // DNS sur AP
    esp_netif_dns_info_t ap_dns;
    if (esp_netif_get_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &ap_dns) == ESP_OK) {
        ESP_LOGI(TAG, "[DIAG-NET] DNS AP : " IPSTR, IP2STR(&ap_dns.ip.u_addr.ip4));
    } else {
        ESP_LOGW(TAG, "[DIAG-NET] DNS AP : non configure");
    }

    // DNS sur PPP
    esp_netif_dns_info_t ppp_dns;
    if (esp_netif_get_dns_info(ppp_netif, ESP_NETIF_DNS_MAIN, &ppp_dns) == ESP_OK) {
        ESP_LOGI(TAG, "[DIAG-NET] DNS PPP : " IPSTR, IP2STR(&ppp_dns.ip.u_addr.ip4));
    } else {
        ESP_LOGW(TAG, "[DIAG-NET] DNS PPP : non configure");
    }

    // Route par defaut lwIP
    struct netif *def_final = netif_default;
    if (def_final) {
        ESP_LOGI(TAG, "[DIAG-NET] Route defaut : '%c%c%d'",
                 def_final->name[0], def_final->name[1], def_final->num);
    } else {
        ESP_LOGW(TAG, "[DIAG-NET] Route defaut : AUCUNE");
    }

    // Liste toutes les netifs
    int nif_count = 0;
    for (struct netif *nif = netif_list; nif != NULL; nif = nif->next) {
        ESP_LOGI(TAG, "[DIAG-NET] netif[%d] '%c%c%d' flags=0x%02x %s",
                 nif_count, nif->name[0], nif->name[1], nif->num,
                 nif->flags,
                 (nif == def_final) ? "<-- DEFAULT" : "");
        nif_count++;
    }

    ESP_LOGI(TAG, "[DIAG-NET] ═══ FIN DUMP RESEAU ═══");
}