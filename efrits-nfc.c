#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
#include <winscard.h>
#define scard_list_readers SCardListReadersA
#define scard_connect SCardConnectA
#else
#include <errno.h>
#include <time.h>
#include <winscard.h>
#define scard_list_readers SCardListReaders
#define scard_connect SCardConnect
#endif

#define EFRITS_NFC_FILE_SIZE 32
#define EFRITS_NFC_TOKEN_SIZE 16
#define EFRITS_NFC_FIRST_PAGE 4
#define EFRITS_NFC_PAGE_SIZE 4
#define EFRITS_NFC_PAGE_COUNT (EFRITS_NFC_FILE_SIZE / EFRITS_NFC_PAGE_SIZE)
#define EFRITS_NFC_CARD_SETTLE_MS 150
#define EFRITS_NFC_IO_RETRY_MS 60
#define EFRITS_NFC_IO_RETRIES 4
#define EFRITS_NFC_WRITE_SETTLE_MS 20
#define EFRITS_NFC_VERIFY_SETTLE_MS 100
#define EFRITS_NFC_SESSION_RETRIES 3
#define EFRITS_NFC_SESSION_RETRY_MS 250

static const unsigned char EFRITS_MAGIC[4] = {'E','F','R','1'};
static const unsigned char EFRITS_TRAILER[4] = {'N','F','C','!'};

static uint32_t crc32_ieee(const unsigned char *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    int bit;

    for (i = 0; i < len; ++i)
    {
        crc ^= data[i];
        for (bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
    return ~crc;
}

static uint32_t read_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static int validate_payload(const unsigned char payload[EFRITS_NFC_FILE_SIZE], char *why, size_t why_size)
{
    uint32_t expected;
    uint32_t actual;

    if (memcmp(payload, EFRITS_MAGIC, sizeof(EFRITS_MAGIC)) != 0)
    {
        snprintf(why, why_size, "signature EFRITS absente");
        return 0;
    }
    if (payload[4] != 1)
    {
        snprintf(why, why_size, "version .nfc non supportee: %u", (unsigned)payload[4]);
        return 0;
    }
    if (payload[5] != EFRITS_NFC_TOKEN_SIZE)
    {
        snprintf(why, why_size, "taille de jeton invalide: %u", (unsigned)payload[5]);
        return 0;
    }
    if (payload[6] != 0 || payload[7] != 0)
    {
        snprintf(why, why_size, "flags/reserve non supportes");
        return 0;
    }
    if (memcmp(payload + 28, EFRITS_TRAILER, sizeof(EFRITS_TRAILER)) != 0)
    {
        snprintf(why, why_size, "marqueur de fin invalide");
        return 0;
    }

    expected = crc32_ieee(payload, 24);
    actual = read_be32(payload + 24);
    if (expected != actual)
    {
        snprintf(why, why_size, "CRC invalide");
        return 0;
    }
    return 1;
}

static char *dup_string(const char *s)
{
    size_t len = strlen(s) + 1;
    char *copy = (char *)malloc(len);
    if (copy)
        memcpy(copy, s, len);
    return copy;
}

static void print_hex(const unsigned char *data, size_t len)
{
    size_t i;
    for (i = 0; i < len; ++i)
    {
        if (i)
            putchar(' ');
        printf("%02X", data[i]);
    }
}

static int has_nfc_extension(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot)
        return 0;
    return tolower((unsigned char)dot[1]) == 'n' &&
           tolower((unsigned char)dot[2]) == 'f' &&
           tolower((unsigned char)dot[3]) == 'c' &&
           dot[4] == '\0';
}

static int load_nfc_file(const char *path, unsigned char payload[EFRITS_NFC_FILE_SIZE])
{
    FILE *fp;
    long size;
    char why[128];

    fp = fopen(path, "rb");
    if (!fp)
    {
        fprintf(stderr, "Impossible d'ouvrir %s.\n", path);
        return 0;
    }
    if (fseek(fp, 0, SEEK_END) != 0 || (size = ftell(fp)) < 0 || fseek(fp, 0, SEEK_SET) != 0)
    {
        fclose(fp);
        fprintf(stderr, "Impossible de lire %s.\n", path);
        return 0;
    }
    if (size != EFRITS_NFC_FILE_SIZE)
    {
        fclose(fp);
        fprintf(stderr, "%s n'est pas un fichier EFRITS NFC v1: taille %ld, attendu %d octets.\n",
                path, size, EFRITS_NFC_FILE_SIZE);
        return 0;
    }
    if (fread(payload, 1, EFRITS_NFC_FILE_SIZE, fp) != EFRITS_NFC_FILE_SIZE)
    {
        fclose(fp);
        fprintf(stderr, "Lecture incomplete de %s.\n", path);
        return 0;
    }
    fclose(fp);

    if (!validate_payload(payload, why, sizeof(why)))
    {
        fprintf(stderr, "%s n'est pas un fichier EFRITS NFC v1 valide: %s.\n", path, why);
        return 0;
    }
    return 1;
}

static void sleep_ms(unsigned long milliseconds)
{
#ifdef _WIN32
    Sleep((DWORD)milliseconds);
#else
    struct timespec req;
    struct timespec rem;

    req.tv_sec = (time_t)(milliseconds / 1000UL);
    req.tv_nsec = (long)((milliseconds % 1000UL) * 1000000UL);
    while (nanosleep(&req, &rem) != 0 && errno == EINTR)
        req = rem;
#endif
}

static const SCARD_IO_REQUEST *pci_for_protocol(DWORD protocol)
{
    if (protocol == SCARD_PROTOCOL_T0)
        return SCARD_PCI_T0;
    return SCARD_PCI_T1;
}

static int transmit_apdu(SCARDHANDLE card, DWORD protocol,
                         const unsigned char *command, DWORD command_len,
                         unsigned char *response, DWORD *response_len)
{
    LONG rc = SCardTransmit(card, pci_for_protocol(protocol), command, command_len,
                            NULL, response, response_len);
    if (rc != SCARD_S_SUCCESS)
    {
        fprintf(stderr, "SCardTransmit a echoue: 0x%08lX\n", (unsigned long)rc);
        return 0;
    }
    if (*response_len < 2)
    {
        fprintf(stderr, "Reponse APDU trop courte.\n");
        return 0;
    }
    if (response[*response_len - 2] != 0x90 || response[*response_len - 1] != 0x00)
    {
        fprintf(stderr, "Commande refusee par la carte/lecteur: SW=%02X%02X\n",
                response[*response_len - 2], response[*response_len - 1]);
        return 0;
    }
    return 1;
}

static char *select_reader(SCARDCONTEXT ctx)
{
    DWORD size = 0;
    char *buffer = NULL;
    char *reader;
    char *fallback = NULL;
    LONG rc;

    rc = scard_list_readers(ctx, NULL, NULL, &size);
    if (rc != SCARD_S_SUCCESS || size <= 1)
    {
        fprintf(stderr, "Aucun lecteur PC/SC disponible (0x%08lX).\n", (unsigned long)rc);
        return NULL;
    }
    buffer = (char *)malloc(size);
    if (!buffer)
        return NULL;
    rc = scard_list_readers(ctx, NULL, buffer, &size);
    if (rc != SCARD_S_SUCCESS)
    {
        free(buffer);
        fprintf(stderr, "Impossible d'enumerer les lecteurs PC/SC: 0x%08lX\n", (unsigned long)rc);
        return NULL;
    }

    for (reader = buffer; *reader; reader += strlen(reader) + 1)
    {
        if (!fallback)
            fallback = reader;
        if (strstr(reader, "ACR1552") || strstr(reader, "acr1552"))
        {
            char *chosen = dup_string(reader);
            free(buffer);
            return chosen;
        }
    }

    if (fallback && buffer[strlen(fallback) + 1] == '\0')
    {
        char *chosen = dup_string(fallback);
        fprintf(stderr, "ACR1552 non trouve; utilisation de l'unique lecteur PC/SC: %s\n", chosen);
        free(buffer);
        return chosen;
    }

    fprintf(stderr, "ACR1552 non trouve. Lecteurs disponibles:\n");
    for (reader = buffer; *reader; reader += strlen(reader) + 1)
        fprintf(stderr, "  - %s\n", reader);
    free(buffer);
    return NULL;
}

static int wait_for_card(SCARDCONTEXT ctx, const char *reader, SCARDHANDLE *card, DWORD *protocol)
{
    LONG rc;
    int announced = 0;

    for (;;)
    {
        rc = scard_connect(ctx, reader, SCARD_SHARE_SHARED,
                           SCARD_PROTOCOL_T0 | SCARD_PROTOCOL_T1,
                           card, protocol);
        if (rc == SCARD_S_SUCCESS)
        {
            /*
             * ACR1552U/Windows can report the card as connectable a little
             * before the PICC side is fully ready for the first APDU.  This
             * happens especially when the program is already waiting and the
             * user then places a card on the reader (the browser workflow).
             */
            sleep_ms(EFRITS_NFC_CARD_SETTLE_MS);
            return 1;
        }
        if (rc != SCARD_E_NO_SMARTCARD && rc != SCARD_W_REMOVED_CARD)
        {
            fprintf(stderr, "Impossible de se connecter a la carte: 0x%08lX\n", (unsigned long)rc);
            return 0;
        }
        if (!announced)
        {
            printf("Posez une carte sur le lecteur...\n");
            fflush(stdout);
            announced = 1;
        }
        sleep_ms(200);
    }
}

static int get_uid(SCARDHANDLE card, DWORD protocol, unsigned char *uid, DWORD *uid_len)
{
    static const unsigned char command[] = {0xFF, 0xCA, 0x00, 0x00, 0x00};
    unsigned char response[64];
    DWORD response_len = sizeof(response);

    if (!transmit_apdu(card, protocol, command, sizeof(command), response, &response_len))
        return 0;
    if (response_len <= 2)
        return 0;
    *uid_len = response_len - 2;
    memcpy(uid, response, *uid_len);
    return 1;
}

static int get_type_a_activation(SCARDHANDLE card, DWORD protocol,
                                 unsigned char *data, DWORD *data_len)
{
    static const unsigned char command[] = {0xFF, 0xCA, 0x02, 0x00, 0x00};
    unsigned char response[64];
    DWORD response_len = sizeof(response);

    if (!transmit_apdu(card, protocol, command, sizeof(command), response, &response_len))
        return 0;
    if (response_len <= 2)
        return 0;
    *data_len = response_len - 2;
    memcpy(data, response, *data_len);
    return 1;
}

static int read_page(SCARDHANDLE card, DWORD protocol, unsigned char page,
                     unsigned char out[EFRITS_NFC_PAGE_SIZE])
{
    unsigned char command[] = {0xFF, 0xB0, 0x00, page, EFRITS_NFC_PAGE_SIZE};
    int attempt;

    for (attempt = 0; attempt < EFRITS_NFC_IO_RETRIES; ++attempt)
    {
        unsigned char response[32];
        DWORD response_len = sizeof(response);

        if (transmit_apdu(card, protocol, command, sizeof(command), response, &response_len))
        {
            if (response_len == EFRITS_NFC_PAGE_SIZE + 2)
            {
                memcpy(out, response, EFRITS_NFC_PAGE_SIZE);
                return 1;
            }
            fprintf(stderr, "Taille inattendue lors de la lecture de la page %u: %lu octets.\n",
                    (unsigned)page, (unsigned long)(response_len - 2));
        }
        if (attempt + 1 < EFRITS_NFC_IO_RETRIES)
            sleep_ms(EFRITS_NFC_IO_RETRY_MS);
    }
    return 0;
}

static int write_page(SCARDHANDLE card, DWORD protocol, unsigned char page,
                      const unsigned char in[EFRITS_NFC_PAGE_SIZE])
{
    unsigned char command[5 + EFRITS_NFC_PAGE_SIZE] = {
        0xFF, 0xD6, 0x00, page, EFRITS_NFC_PAGE_SIZE, 0, 0, 0, 0
    };
    int attempt;

    memcpy(command + 5, in, EFRITS_NFC_PAGE_SIZE);
    for (attempt = 0; attempt < EFRITS_NFC_IO_RETRIES; ++attempt)
    {
        unsigned char response[32];
        DWORD response_len = sizeof(response);

        if (transmit_apdu(card, protocol, command, sizeof(command), response, &response_len))
        {
            /* Ultralight/Type-2 writes commit to EEPROM.  Give the tag a tiny
             * amount of time before the next page or the read-back check. */
            sleep_ms(EFRITS_NFC_WRITE_SETTLE_MS);
            return 1;
        }
        if (attempt + 1 < EFRITS_NFC_IO_RETRIES)
            sleep_ms(EFRITS_NFC_IO_RETRY_MS);
    }
    return 0;
}

static int read_payload(SCARDHANDLE card, DWORD protocol,
                        unsigned char payload[EFRITS_NFC_FILE_SIZE])
{
    int i;
    for (i = 0; i < EFRITS_NFC_PAGE_COUNT; ++i)
        if (!read_page(card, protocol, (unsigned char)(EFRITS_NFC_FIRST_PAGE + i),
                       payload + i * EFRITS_NFC_PAGE_SIZE))
            return 0;
    return 1;
}

static int card_is_safe_page_tag(SCARDHANDLE card, DWORD protocol)
{
    unsigned char activation[64];
    DWORD activation_len = sizeof(activation);
    unsigned char sak;

    if (!get_type_a_activation(card, protocol, activation, &activation_len))
    {
        fprintf(stderr, "Impossible de verifier le type de carte; ecriture refusee.\n");
        return 0;
    }
    if (activation_len < 4)
    {
        fprintf(stderr, "Donnees d'activation Type A trop courtes; ecriture refusee.\n");
        return 0;
    }
    sak = activation[activation_len - 1];
    if (sak != 0x00)
    {
        fprintf(stderr, "SAK=%02X: ce support ne ressemble pas a un tag Ultralight/Type-2. Ecriture refusee.\n", sak);
        return 0;
    }
    return 1;
}

static int do_read(SCARDHANDLE card, DWORD protocol)
{
    unsigned char uid[32];
    DWORD uid_len = sizeof(uid);
    unsigned char payload[EFRITS_NFC_FILE_SIZE];
    char why[128];

    if (!get_uid(card, protocol, uid, &uid_len))
        return 0;
    printf("UID : ");
    print_hex(uid, uid_len);
    putchar('\n');

    if (!read_payload(card, protocol, payload))
    {
        fprintf(stderr, "La carte ne permet pas la lecture des pages EFRITS 4..11 avec ce protocole.\n");
        return 0;
    }
    if (!validate_payload(payload, why, sizeof(why)))
    {
        printf("Carte EFRITS : non (%s)\n", why);
        printf("Pages 4..11 : ");
        print_hex(payload, sizeof(payload));
        putchar('\n');
        return 1;
    }

    printf("Carte EFRITS : oui, format v1\n");
    printf("Jeton : ");
    print_hex(payload + 8, EFRITS_NFC_TOKEN_SIZE);
    putchar('\n');
    printf("CRC : OK\n");
    return 1;
}

static int do_write(SCARDHANDLE card, DWORD protocol,
                    const unsigned char payload[EFRITS_NFC_FILE_SIZE])
{
    unsigned char before[EFRITS_NFC_FILE_SIZE];
    unsigned char after[EFRITS_NFC_FILE_SIZE];
    char why[128];
    int i;

    if (!card_is_safe_page_tag(card, protocol))
        return 0;

    if (!read_payload(card, protocol, before))
    {
        fprintf(stderr, "Impossible de verifier les pages 4..11; ecriture refusee.\n");
        return 0;
    }

    if (validate_payload(before, why, sizeof(why)))
    {
        if (memcmp(before, payload, EFRITS_NFC_FILE_SIZE) == 0)
        {
            printf("La carte contient deja exactement ce fichier EFRITS NFC. Rien a faire.\n");
            return 1;
        }
        printf("Une autre carte EFRITS v1 est deja programmee sur ce support; elle va etre remplacee.\n");
    }

    for (i = 0; i < EFRITS_NFC_PAGE_COUNT; ++i)
    {
        unsigned char page = (unsigned char)(EFRITS_NFC_FIRST_PAGE + i);
        if (!write_page(card, protocol, page, payload + i * EFRITS_NFC_PAGE_SIZE))
        {
            fprintf(stderr, "Echec d'ecriture a la page %u.\n", (unsigned)page);
            return 0;
        }
    }

    /* The final write may have succeeded before the reader/tag has finished
     * settling.  The CLI used to report a false failure here even though a
     * subsequent standalone read showed the freshly programmed payload. */
    sleep_ms(EFRITS_NFC_VERIFY_SETTLE_MS);
    if (!read_payload(card, protocol, after))
    {
        fprintf(stderr, "Ecriture terminee mais relecture de verification impossible.\n");
        return 0;
    }
    if (memcmp(payload, after, EFRITS_NFC_FILE_SIZE) != 0)
    {
        fprintf(stderr, "ERREUR: la relecture ne correspond pas au fichier .nfc.\n");
        return 0;
    }
    if (!validate_payload(after, why, sizeof(why)))
    {
        fprintf(stderr, "ERREUR: le contenu relu est invalide: %s.\n", why);
        return 0;
    }

    printf("Carte EFRITS programmee et verifiee.\n");
    printf("Jeton : ");
    print_hex(after + 8, EFRITS_NFC_TOKEN_SIZE);
    putchar('\n');
    return 1;
}

int main(int argc, char **argv)
{
    unsigned char payload[EFRITS_NFC_FILE_SIZE];
    int writing = argc == 2;
    SCARDCONTEXT ctx;
    SCARDHANDLE card = 0;
    DWORD protocol = 0;
    char *reader = NULL;
    LONG rc;
    int ok = 0;

    if (argc > 2)
    {
        fprintf(stderr, "Usage: %s [fichier.nfc]\n", argv[0]);
        return 2;
    }

    /* Requested behaviour: a non-.nfc argument is ignored completely. */
    if (writing && !has_nfc_extension(argv[1]))
        return 0;

    if (writing && !load_nfc_file(argv[1], payload))
        return 1;

    rc = SCardEstablishContext(SCARD_SCOPE_USER, NULL, NULL, &ctx);
    if (rc != SCARD_S_SUCCESS)
    {
        fprintf(stderr, "Impossible d'ouvrir PC/SC: 0x%08lX\n", (unsigned long)rc);
#ifndef _WIN32
#ifdef SCARD_E_NO_SERVICE
        if (rc == SCARD_E_NO_SERVICE)
            fprintf(stderr, "Le service pcscd ne semble pas disponible. Essayez: sudo systemctl start pcscd.socket\n");
#endif
#endif
        return 1;
    }

    reader = select_reader(ctx);
    if (!reader)
        goto cleanup;
    printf("Lecteur : %s\n", reader);

    {
        int attempt;

        for (attempt = 0; attempt < EFRITS_NFC_SESSION_RETRIES && !ok; ++attempt)
        {
            if (!wait_for_card(ctx, reader, &card, &protocol))
                break;

            if (writing)
            {
                unsigned char uid[32];
                DWORD uid_len = sizeof(uid);
                if (get_uid(card, protocol, uid, &uid_len))
                {
                    printf("UID : ");
                    print_hex(uid, uid_len);
                    putchar('\n');
                }
                ok = do_write(card, protocol, payload);
            }
            else
                ok = do_read(card, protocol);

            if (!ok && attempt + 1 < EFRITS_NFC_SESSION_RETRIES)
            {
                /*
                 * Some ACR1552U/PICC failures poison the current PC/SC
                 * session: retrying the same APDU on the same handle does not
                 * recover, while a fresh CLI invocation immediately works.
                 * Reproduce that recovery here by resetting and reconnecting
                 * the card before retrying the complete operation.  A write
                 * is idempotent for our 32-byte payload: if the first attempt
                 * actually committed all pages but only its verification
                 * failed, the next attempt notices that the expected payload
                 * is already present and returns success.
                 */
                fprintf(stderr,
                        "Operation NFC non confirmee; nouvelle tentative avec une nouvelle session PC/SC (%d/%d).\n",
                        attempt + 2, EFRITS_NFC_SESSION_RETRIES);
                fflush(stdout);
                fflush(stderr);
                SCardDisconnect(card, SCARD_RESET_CARD);
                card = 0;
                protocol = 0;
                sleep_ms(EFRITS_NFC_SESSION_RETRY_MS);
            }
        }
    }

cleanup:
    if (card)
        SCardDisconnect(card, SCARD_LEAVE_CARD);
    free(reader);
    SCardReleaseContext(ctx);
    return ok ? 0 : 1;
}

