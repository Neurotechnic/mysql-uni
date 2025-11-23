#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mysql/mysql.h>
#include <windows.h>
#include <conio.h>
#include "myfunc.h"

// ANSI-Farbsequenzen
#define COLOR_RESET     "\033[0m"
#define COLOR_GRAY      "\033[90m"
#define COLOR_WHITE     "\033[37m"
#define COLOR_CYAN      "\033[36m"
#define COLOR_CYANH      "\033[96m"
#define COLOR_YELLOW    "\033[33m"
#define COLOR_YELLOW_I  "\033[30;43m"
#define COLOR_MAGENTA   "\033[35m"
#define COLOR_MAGENTAH   "\033[95m"
#define COLOR_GREEN     "\033[32m"
#define COLOR_RED       "\033[31m"

MYSQL *conn = NULL;
char monat[8]="\0";              // z.B. "2025-02"
char server[256];
char user[256];
char password[256];
char database[256];

//Definitionen für dynamisches menü
typedef void (*MenuCallback)(const char *context);

typedef struct {
    char title[128];       
    MenuCallback callback; 
    char context[4096];    
} MenuItem;

MenuItem *g_menuItems = NULL;
int g_menuCount = 0;
int g_menuCapacity = 0;       // Wie viel Platz haben wir aktuell reserviert?

typedef struct ConfigNode {
    char type[10]; // "TABLE" oder "REPORT"
    char key[128];
    char value[4096];
    struct ConfigNode *next;
} ConfigNode;

ConfigNode *g_configList = NULL;
/////////////////////

long InteractiveTable(const char *tableName, int selectionMode);
int DrawReport(const char *title, const char *sqlQuery);
void AddPaymentProcess(void);
void Verkaufen(void);

//Callback wrapper funktionen

void cb_Table(const char *tableName) {
    InteractiveTable(tableName, 0);
}

void cb_Report(const char *context) {
    // Format: "Titel:SQL"
    const char *sep = strchr(context, ':');
    
    if (sep) {
        // Titel extrahieren (linker Teil)
        char title[256];
        size_t titleLen = sep - context;
        if (titleLen >= sizeof(title)) titleLen = sizeof(title) - 1;
        strncpy(title, context, titleLen);
        title[titleLen] = '\0';
        
        // SQL beginnt nach dem Separator (rechter Teil)
        const char *sql = sep + 1;
        
        DrawReport(title, sql);
    } else {
        DrawReport("", context);
    }
}

void cb_Payment(const char *dummy) {
    (void)dummy;
    AddPaymentProcess();
}

void cb_Verkaufen(const char *dummy) {
    (void)dummy;
    Verkaufen();
}

void cb_Exit(const char *dummy) {
    (void)dummy;
}

// Zeilenumbruch am Ende eines Strings entfernen
void trim_newline(char *str) {
    if (!str) return;
    str[strcspn(str, "\r\n")] = 0;
}

// Länge eines UTF-8-Strings in Zeichen (nicht Bytes) bestimmen
int utf8_strlen(const char *utf8str) {
    if (!utf8str) return 0;
    int len_utf16 = MultiByteToWideChar(CP_UTF8, 0, utf8str, -1, NULL, 0);
    if (len_utf16 <= 0) return 0;
    return len_utf16 - 1;
}

// UTF-8-String ausgeben und mit Leerzeichen auffüllen
void print_utf8_padded(const char *s, int width) {
    if (!s) s = "NULL";
    int disp = utf8_strlen(s);
    printf("%s", s);
    for (int i = disp; i < width; i++) putchar(' ');
}

// Zeile von stdin lesen und \r\n entfernen
void readLine(char *buf, size_t size) {
    if (!fgets(buf, (int)size, stdin)) {
        buf[0] = '\0';
        return;
    }
    trim_newline(buf);
}

// Hilfsfunktion: Case-insensitive String-Vergleich für den Anfang eines Strings
int startsWithIgnoreCase(const char *str, const char *prefix) {
    while (*prefix) {
        if (tolower(*str) != tolower(*prefix)) return 0;
        str++;
        prefix++;
    }
    return 1;
}

// Datentyp-Helfer (für Validierung)
int isNumericType(enum enum_field_types t) {
    switch (t) {
        case MYSQL_TYPE_TINY:
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_LONGLONG:
        case MYSQL_TYPE_DECIMAL:
        case MYSQL_TYPE_NEWDECIMAL:
        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DOUBLE:
            return 1;
        default:
            return 0;
    }
}

int isDateType(enum enum_field_types t) {
    switch (t) {
        case MYSQL_TYPE_DATE:
        case MYSQL_TYPE_DATETIME:
        case MYSQL_TYPE_TIMESTAMP:
            return 1;
        default:
            return 0;
    }
}

// Ganzzahl / Dezimalzahl prüfen (einfach)
int validateNumericInput(const char *s) {
    if (!s || s[0] == '\0') return 1;
    int dotCount = 0;
    int i = 0;
    if (s[0] == '-') i = 1;
    for (; s[i] != '\0'; i++) {
        if (s[i] == '.') {
            if (++dotCount > 1) return 0;
        } else if (s[i] < '0' || s[i] > '9') {
            return 0;
        }
    }
    return 1;
}

// Datum im Format JJJJ-MM-TT prüfen
int validateDateInput(const char *s) {
    if (!s || s[0] == '\0') return 1; // leer zulassen
    if (strlen(s) != 10) return 0;
    if (s[4] != '-' || s[7] != '-') return 0;
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) continue;
        if (s[i] < '0' || s[i] > '9') return 0;
    }
    return 1;
}

// Fremdschlüssel-Informationen aus INFORMATION_SCHEMA
// Liefert zu (tableName, columnName) die referenzierte Tabelle/Spalte,
// falls es sich um einen Fremdschlüssel handelt.
// Rückgabe: 1 = ist FK, 0 = kein FK oder Fehler.
int getForeignKeyInfo(const char *tableName, const char *columnName,
                      char *refTable, size_t refTableSize,
                      char *refColumn, size_t refColumnSize) {
    char q[512];
    snprintf(q, sizeof(q),
             "SELECT REFERENCED_TABLE_NAME, REFERENCED_COLUMN_NAME "
             "FROM INFORMATION_SCHEMA.KEY_COLUMN_USAGE "
             "WHERE TABLE_SCHEMA='%s' "
             "AND TABLE_NAME='%s' "
             "AND COLUMN_NAME='%s' "
             "AND REFERENCED_TABLE_NAME IS NOT NULL "
             "LIMIT 1;",
             database, tableName, columnName);

    if (mysql_query(conn, q) != 0) {
        return 0;
    }
    MYSQL_RES *res = mysql_store_result(conn);
    if (!res) return 0;

    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row || !row[0] || !row[1]) {
        mysql_free_result(res);
        return 0;
    }

    strncpy(refTable, row[0], refTableSize - 1);
    refTable[refTableSize - 1] = '\0';
    strncpy(refColumn, row[1], refColumnSize - 1);
    refColumn[refColumnSize - 1] = '\0';

    mysql_free_result(res);
    return 1;
}

// Prüfen, ob der eingegebene Wert in der referenzierten Tabelle existiert
int validateForeignKey(const char *tableName, const MYSQL_FIELD *field, const char *value) {
    if (!value || value[0] == '\0') return 1; // NULL ist hier erlaubt

    char refTable[128];
    char refColumn[128];
    if (!getForeignKeyInfo(tableName, field->name, refTable, sizeof(refTable), refColumn, sizeof(refColumn))) {
        // kein Fremdschlüssel
        return 1;
    }

    char q[512];
    if (isNumericType(field->type)) {
        snprintf(q, sizeof(q),
                 "SELECT 1 FROM %s WHERE %s=%s LIMIT 1;",
                 refTable, refColumn, value);
    } else {
        char esc[256];
        unsigned long len = mysql_real_escape_string(
            conn, esc, value, (unsigned long)strlen(value));
        esc[len] = '\0';
        snprintf(q, sizeof(q),
                 "SELECT 1 FROM %s WHERE %s='%s' LIMIT 1;",
                 refTable, refColumn, esc);
    }

    if (mysql_query(conn, q) != 0) {
        return 0;
    }
    MYSQL_RES *res = mysql_store_result(conn);
    if (!res) return 0;

    MYSQL_ROW row = mysql_fetch_row(res);
    int ok = (row != NULL);
    mysql_free_result(res);
    return ok;
}

// Textvorschau zu einem Fremdschlüssel-Wert erzeugen, z.B.
int buildForeignKeyPreview(const char *tableName, const MYSQL_FIELD *field, const char *value, char *out, size_t outSize) {
    out[0] = '\0';
    if (!value || value[0] == '\0') return 0;

    char refTable[128];
    char refColumn[128];
    if (!getForeignKeyInfo(tableName, field->name, refTable, sizeof(refTable), refColumn, sizeof(refColumn))) {
        return 0; // kein FK
    }

    char q[512];
    if (isNumericType(field->type)) {
        snprintf(q, sizeof(q), "SELECT * FROM %s WHERE %s=%s LIMIT 1;", refTable, refColumn, value);
    } else {
        char esc[256];
        unsigned long len = mysql_real_escape_string(conn, esc, value, (unsigned long)strlen(value));
        esc[len] = '\0';
        snprintf(q, sizeof(q), "SELECT * FROM %s WHERE %s='%s' LIMIT 1;", refTable, refColumn, esc);
    }

    if (mysql_query(conn, q) != 0) return 0;
    MYSQL_RES *res = mysql_store_result(conn);
    if (!res) return 0;

    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) {
        mysql_free_result(res);
        return 0;
    }

    int num_fields = mysql_num_fields(res);
    char buf[512] = "";
    for (int i = 1; i < num_fields; i++) {
        if (i > 1) strncat(buf, " │ ", sizeof(buf) - strlen(buf) - 1);
        if (row[i]) {
            strncat(buf, row[i], sizeof(buf) - strlen(buf) - 1);
        } else {
            strncat(buf, "NULL", sizeof(buf) - strlen(buf) - 1);
        }
    }
    mysql_free_result(res);

    snprintf(out, outSize, "%s: " COLOR_GRAY "%s" COLOR_RESET, value, buf);
    return 1;
}

// Benutzereingabe für ein Feld mit Typ- und FK-Prüfung
void inputValueForField(const char *tableName, const MYSQL_FIELD *field, const char *oldValue, char *buffer, size_t bufSize, int allowEmpty) {
    (void)oldValue; // momentan nicht genutzt, aber für spätere Erweiterung reserviert

    while (1) {
        buffer[0] = '\0';
        readLine(buffer, bufSize);

        if (buffer[0] == '\0') {
            if (allowEmpty) return; // leer = unverändert / NULL
            printf(COLOR_RED "Eingabe darf nicht leer sein. Bitte erneut eingeben: " COLOR_RESET);
            continue;
        }

        // Typprüfung
        if (isNumericType(field->type)) {
            if (!validateNumericInput(buffer)) {
                printf(COLOR_RED "Ungültige Zahl. Bitte erneut eingeben: " COLOR_RESET);
                continue;
            }
        } else if (isDateType(field->type)) {
            if (!validateDateInput(buffer)) {
                printf(COLOR_RED "Ungültiges Datum (Format JJJJ-MM-TT). Bitte erneut eingeben: " COLOR_RESET);
                continue;
            }
        }

        // Fremdschlüssel prüfen
        if (!validateForeignKey(tableName, field, buffer)) {
            printf(COLOR_RED "Ungültiger Fremdschlüssel (kein passender Datensatz). Bitte erneut eingeben: " COLOR_RESET);
            continue;
        }

        return;
    }
}

// Hauptmenü
void AddMenuItem(const char *title, MenuCallback callback, const char *context) {
    // Wenn voll, vergrößern (Capacity verdoppeln oder auf 10 setzen)
    if (g_menuCount >= g_menuCapacity) {
        int newCapacity = (g_menuCapacity == 0) ? 10 : g_menuCapacity * 2;
        
        MenuItem *newPtr = (MenuItem*)realloc(g_menuItems, newCapacity * sizeof(MenuItem));
        if (!newPtr) {
            fprintf(stderr, "Kritischer Fehler: Nicht genügend Arbeitsspeicher!\n");
            return;
        }
        g_menuItems = newPtr;
        g_menuCapacity = newCapacity;
    }

    // Daten kopieren
    strncpy(g_menuItems[g_menuCount].title, title, sizeof(g_menuItems[g_menuCount].title) - 1);
    g_menuItems[g_menuCount].title[sizeof(g_menuItems[g_menuCount].title) - 1] = '\0'; // Safety NULL
    
    g_menuItems[g_menuCount].callback = callback;
    
    if (context) {
        strncpy(g_menuItems[g_menuCount].context, context, sizeof(g_menuItems[g_menuCount].context) - 1);
        g_menuItems[g_menuCount].context[sizeof(g_menuItems[g_menuCount].context) - 1] = '\0';
    } else {
        g_menuItems[g_menuCount].context[0] = '\0';
    }
    
    g_menuCount++;
}

void AddConfigNode(const char *type, const char *key, const char *val) {
    ConfigNode *node = (ConfigNode*)malloc(sizeof(ConfigNode));
    if (!node) return;
    strcpy(node->type, type);
    strcpy(node->key, key ? key : "");
    strcpy(node->value, val ? val : "");
    node->next = NULL;

    if (!g_configList) {
        g_configList = node;
    } else {
        ConfigNode *curr = g_configList;
        while(curr->next) curr = curr->next;
        curr->next = node;
    }
}

void DrawMainMenu(int selected) {
    system("cls");
    printf(COLOR_CYAN "Hauptmenü\n\n" COLOR_RESET);
    
    for (int i = 0; i < g_menuCount; i++) {
        if (i == selected) {
            printf(COLOR_YELLOW_I "> %s\n" COLOR_RESET, g_menuItems[i].title);
        } else {
            printf("  %s\n", g_menuItems[i].title);
        }
    }
    
    printf("\n" COLOR_GRAY
           "Pfeiltasten: Wählen | ENTER: Öffnen | Strg+C: Beenden\n"
           COLOR_RESET);
}

// Interaktive Tabellenansicht mit Paging
// Zeichnet die Tabelle mit max. pageSize Zeilen pro Seite.
// res       : bereits geladenes Ergebnis von SELECT * FROM tableName
// page      : aktuelle Seite (0-basiert)
// selInPage : Index der markierten Zeile innerhalb der Seite
// totalRowsOut : Gesamtanzahl der Datensätze
// idFieldNameOut / selectedIdOut : Info zum Primärschlüssel der markierten Zeile
// selectionMode: 0 = Normal (Anzeigen/Editieren), 1 = Auswahlmodus (Enter = Wählen)
int DrawInteractiveTable(MYSQL_RES *res, const char *tableName, int page, int pageSize, int selInPage, int *totalRowsOut, char *idFieldNameOut, size_t idFieldNameSize,
                         char *selectedIdOut, size_t selectedIdSize, int selectionMode) {
    if (!res) return 1;

    int num_fields = mysql_num_fields(res);
    MYSQL_FIELD *fields = mysql_fetch_fields(res);
    unsigned int col_widths[num_fields];

    for (int i = 0; i < num_fields; i++) {
        unsigned int header_len = utf8_strlen(fields[i].name);
        unsigned int data_len   = fields[i].max_length;
        col_widths[i] = (header_len > data_len ? header_len : data_len) + 2;
    }

    int totalRows = (int)mysql_num_rows(res);
    if (totalRowsOut) *totalRowsOut = totalRows;

    // ID-Feldname (erste Spalte)
    if (num_fields > 0) {
        strncpy(idFieldNameOut, fields[0].name, idFieldNameSize - 1);
        idFieldNameOut[idFieldNameSize - 1] = '\0';
    } else {
        idFieldNameOut[0] = '\0';
    }

    int totalPages = (totalRows + pageSize - 1) / pageSize;
    if (totalPages == 0) totalPages = 1;
    if (page < 0) page = 0;
    if (page >= totalPages) page = totalPages - 1;

    int startIndex = page * pageSize;
    int endIndex   = startIndex + pageSize;
    if (endIndex > totalRows) endIndex = totalRows;

    int globalSelectedIndex = startIndex + selInPage;
    if (globalSelectedIndex >= endIndex) globalSelectedIndex = endIndex - 1;
    if (globalSelectedIndex < startIndex && totalRows > 0) globalSelectedIndex = startIndex;

    system("cls");
    if (selectionMode) {
        printf(COLOR_GREEN "BITTE DATENSATZ AUSWÄHLEN: %s\n" COLOR_RESET, tableName);
    } else {
        printf(COLOR_CYAN "Tabelle: %s\n" COLOR_RESET, tableName);
    }

    if (totalRows > 0) {
        int shownStart = startIndex + 1;
        int shownEnd   = endIndex;
        printf(COLOR_GRAY
               "Seite %d/%d – Datensätze %d–%d von %d\n"
               COLOR_RESET,
               page + 1, totalPages, shownStart, shownEnd, totalRows);
    } else {
        printf(COLOR_GRAY "Keine Datensätze vorhanden.\n" COLOR_RESET);
    }
    
    // Angepasste Hilfe
    if (selectionMode) {
        printf(COLOR_CYANH "ENTER: Auswählen" COLOR_GRAY "   Pfeile: Navigieren   f: filtern   ESC: Abbrechen\n\n" COLOR_RESET);
    } else {
        printf(COLOR_GRAY "Pfeile: Navigieren   w: bearbeiten   a: neu   f: filtern   ESC: zurück\n\n" COLOR_RESET);
    }

    // obere Linie
    printf("┌");
    for (int i = 0; i < num_fields; i++) {
        for (int j = 0; j <= (int)col_widths[i]; ++j) printf("─");
        if (i == num_fields - 1) printf("┐");
        else printf("┬");
    }
    printf("\n");

    // Kopfzeile
    printf("│ ");
    for (int i = 0; i < num_fields; i++) {
        printf("%-*s", (int)col_widths[i], fields[i].name);
        printf("│ ");
    }
    printf("\n");

    // Trennlinie
    printf("├");
    for (int i = 0; i < num_fields; i++) {
        for (int j = 0; j <= (int)col_widths[i]; ++j) printf("─");
        if (i == num_fields - 1) printf("┤");
        else printf("┼");
    }
    printf("\n");

    selectedIdOut[0] = '\0';

    for (int rowIndex = startIndex; rowIndex < endIndex; ++rowIndex) {
        mysql_data_seek(res, rowIndex);
        MYSQL_ROW row = mysql_fetch_row(res);
        if (!row) break;

        int isSelected = (rowIndex == globalSelectedIndex);
        if (isSelected) printf(COLOR_YELLOW_I);

        printf("│ ");
        for (int i = 0; i < num_fields; i++) {
            print_utf8_padded(row[i], col_widths[i]);
            printf("│ ");
        }
        printf("\n");

        if (isSelected) {
            printf(COLOR_RESET);
            if (row[0]) {
                strncpy(selectedIdOut, row[0], selectedIdSize - 1);
                selectedIdOut[selectedIdSize - 1] = '\0';
            }
        }
    }

    // untere Linie
    printf("└");
    for (int i = 0; i < num_fields; i++) {
        for (int j = 0; j <= (int)col_widths[i]; ++j) printf("─");
        if (i == num_fields - 1) printf("┘");
        else printf("┴");
    }
    printf("\n");

    return 0;
}

// Eingabe für Fremdschlüssel-Felder über InteractiveTable
// Rückgabe:
//   0 -> kein Fremdschlüssel, Aufrufer soll normale Eingabe machen
//   1 -> handled; buffer enthält entweder ausgewählten ID oder "" (unverändert/NULL)
int inputForeignKeyViaTable(const char *tableName, const MYSQL_FIELD *field, const char *oldValue, char *buffer, size_t bufSize, int allowEmpty) {
    (void)oldValue; // evtl. später für Default-Anzeige

    char refTable[128];
    char refColumn[128];

    // Ist dieses Feld überhaupt ein Fremdschlüssel?
    if (!getForeignKeyInfo(tableName, field->name, refTable, sizeof(refTable), refColumn, sizeof(refColumn)))
    {
        return 0; // kein FK -> Aufrufer macht normale Eingabe
    }

    while (1) {
        printf(COLOR_GRAY "(Fremdschluessel -> %s.%s, Auswahl mit Pfeiltasten + ENTER,\n"
               " ESC im Auswahlfenster = unveraendert/NULL)\n" COLOR_RESET, refTable, refColumn);

        long selId = InteractiveTable(refTable, 1);

        if (selId <= 0) {
            // Benutzer hat ESC gedrückt oder nichts gewählt
            if (allowEmpty) {
                buffer[0] = '\0';   // bedeutet: unverändert (Edit) oder NULL (Insert)
                return 1;
            } else {
                printf(COLOR_RED "Dieses Feld darf nicht leer sein. Bitte erneut waehlen.\n" COLOR_RESET);
                continue;
            }
        }

        // ID als Text speichern
        snprintf(buffer, bufSize, "%ld", selId);

        // zur Sicherheit noch einmal FK prüfen (sollte immer OK sein)
        if (!validateForeignKey(tableName, field, buffer)) {
            printf(COLOR_RED "Ungueltiger Fremdschluessel (kein passender Datensatz). Bitte erneut waehlen.\n"
                   COLOR_RESET);
            continue;
        }

        return 1;
    }
}

// Datensätze bearbeiten / hinzufügen
int EditRecord(const char *tableName, const char *idFieldName, const char *idValue) {
    if (!idFieldName[0] || !idValue[0]) {
        printf(COLOR_RED "Keine Zeile ausgewählt.\n" COLOR_RESET);
        getch();
        return 1;
    }

    char escId[256];
    unsigned long lenId = mysql_real_escape_string(
        conn, escId, idValue, (unsigned long)strlen(idValue));
    escId[lenId] = '\0';

    char q[512];
    snprintf(q, sizeof(q),
             "SELECT * FROM %s WHERE %s='%s';",
             tableName, idFieldName, escId);

    if (mysql_query(conn, q) != 0) {
        fprintf(stderr, "Abfrage fehlgeschlagen: %s\n", mysql_error(conn));
        getch();
        return 1;
    }

    MYSQL_RES *res = mysql_store_result(conn);
    if (!res) {
        fprintf(stderr, "Fehler beim Lesen des Ergebnisses: %s\n", mysql_error(conn));
        getch();
        return 1;
    }

    int num_fields = mysql_num_fields(res);
    MYSQL_FIELD *fields = mysql_fetch_fields(res);
    MYSQL_ROW row = mysql_fetch_row(res);

    if (!row) {
        printf(COLOR_RED "Datensatz nicht gefunden.\n" COLOR_RESET);
        mysql_free_result(res);
        getch();
        return 1;
    }

    // Bildschirm / UI
    system("cls");
    printf(COLOR_CYAN "Datensatz bearbeiten – Tabelle %s\n" COLOR_RESET, tableName);
    printf(COLOR_GRAY "%s = %s\n\n" COLOR_RESET, idFieldName, idValue);
    printf("Leere Eingabe = Wert unverändert lassen.\n\n");

    // Speicher für neue Werte
    char **newValues = (char**)calloc(num_fields, sizeof(char*));

    //  Eingabeschleife für alle Felder außer PK (0)
    for (int i = 1; i < num_fields; ++i) {

        // Fremdschlüssel-Vorschau generieren
        char fkPreview[512];
        fkPreview[0] = '\0';
        buildForeignKeyPreview(tableName, &fields[i], row[i], fkPreview, sizeof(fkPreview));

        // Anzeige des alten Wertes
        if (fkPreview[0]) {
            printf(COLOR_WHITE "%s" COLOR_RESET " [%s]: ", fields[i].name, fkPreview);
        } else {
            printf(COLOR_WHITE "%s" COLOR_RESET " [%s]: ", fields[i].name, row[i] ? row[i] : "NULL");
        }

        // Eingabe
        char buf[512];
        inputValueForField(tableName, &fields[i], row[i], buf, sizeof(buf), 1);

        // Wenn leer → unverändert
        if (buf[0] != '\0')
            newValues[i] = _strdup(buf);
    }

    // UPDATE zusammenbauen
    int changes = 0;
    char query[4096];
    strcpy(query, "UPDATE ");
    strcat(query, tableName);
    strcat(query, " SET ");

    for (int i = 1; i < num_fields; ++i) {
        if (newValues[i]) {

            if (changes > 0) strcat(query, ", ");

            strcat(query, fields[i].name);
            strcat(query, "=");

            if (isNumericType(fields[i].type)) {
                // Zahl → direkt
                strcat(query, newValues[i]);
            }
            else if (isDateType(fields[i].type)) {
                // Datum → in '...'
                strcat(query, "'");
                strcat(query, newValues[i]);
                strcat(query, "'");
            }
            else {
                // Text → escapen
                char esc[512];
                unsigned long len = mysql_real_escape_string(
                    conn, esc, newValues[i],
                    (unsigned long)strlen(newValues[i]));
                esc[len] = '\0';

                strcat(query, "'");
                strcat(query, esc);
                strcat(query, "'");
            }

            ++changes;
        }
    }

    if (changes == 0) {
        printf("\n" COLOR_GRAY "Keine Änderungen vorgenommen.\n" COLOR_RESET);

        for (int i = 0; i < num_fields; ++i)
            if (newValues[i]) free(newValues[i]);
        free(newValues);
        mysql_free_result(res);
        getch();
        return 0;
    }

    // WHERE-Klausel anhängen (immer als Text, MySQL castet selbst)
    strcat(query, " WHERE ");
    strcat(query, idFieldName);
    strcat(query, "='");
    strcat(query, escId);
    strcat(query, "';");

    // Clean up
    mysql_free_result(res);
    for (int i = 0; i < num_fields; ++i)
        if (newValues[i]) free(newValues[i]);
    free(newValues);

    // UPDATE ausführen
    if (mysql_query(conn, query) != 0) {
        fprintf(stderr, "UPDATE fehlgeschlagen: %s\n", mysql_error(conn));
        getch();
        return 1;
    }

    // Erfolgsmeldung + SQL-Befehl anzeigen (Lernzweck)
    printf("\n" COLOR_GREEN "Datensatz wurde aktualisiert.\n" COLOR_RESET);
    printf(COLOR_CYANH "SQL: %s\n" COLOR_RESET, query);
    getch();

    return 0;
}


int AddRecord(const char *tableName) {
    char q[256];
    snprintf(q, sizeof(q), "SELECT * FROM %s LIMIT 1;", tableName);

    if (mysql_query(conn, q) != 0) {
        fprintf(stderr, "Abfrage fehlgeschlagen: %s\n", mysql_error(conn));
        getch();
        return 1;
    }
    MYSQL_RES *res = mysql_store_result(conn);
    if (!res) {
        fprintf(stderr, "Fehler beim Lesen des Ergebnisses: %s\n", mysql_error(conn));
        getch();
        return 1;
    }

    int num_fields = mysql_num_fields(res);
    MYSQL_FIELD *fields = mysql_fetch_fields(res);

    system("cls");
    printf(COLOR_CYAN "Neuen Datensatz anlegen - Tabelle %s\n" COLOR_RESET, tableName);
    printf("Leere Eingabe = NULL.\n\n");

    char **values = (char**)calloc(num_fields, sizeof(char*));

    fflush(stdin);

    for (int i = 1; i < num_fields; i++) { // Spalte 0 = ID (Auto-Increment)
        printf(COLOR_WHITE "%s" COLOR_RESET ": ", fields[i].name);
        char buf[512];
        inputValueForField(tableName, &fields[i], NULL, buf, sizeof(buf), 1);

        if (buf[0] != '\0') {
            values[i] = _strdup(buf);
        } else {
            values[i] = NULL; // NULL schreiben
        }
    }

    char query[4096];
    strcpy(query, "INSERT INTO ");
    strcat(query, tableName);
    strcat(query, " (");

    int colCount = 0;
    for (int i = 1; i < num_fields; i++) {
        if (colCount > 0) strcat(query, ", ");
        strcat(query, fields[i].name);
        ++colCount;
    }
    strcat(query, ") VALUES (");

    colCount = 0;
    for (int i = 1; i < num_fields; i++) {
        if (colCount > 0) strcat(query, ", ");

        if (!values[i]) {
            strcat(query, "NULL");
        } else if (isNumericType(fields[i].type)) {
            strcat(query, values[i]);
        } else if (isDateType(fields[i].type)) {
            strcat(query, "'");
            strcat(query, values[i]);
            strcat(query, "'");
        } else {
            char esc[512];
            unsigned long len = mysql_real_escape_string(conn, esc, values[i], (unsigned long)strlen(values[i]));
            esc[len] = '\0';
            strcat(query, "'");
            strcat(query, esc);
            strcat(query, "'");
        }
        ++colCount;
    }
    strcat(query, ");");

    mysql_free_result(res);
    for (int i = 0; i < num_fields; i++)
        if (values[i]) free(values[i]);
    free(values);

    if (mysql_query(conn, query) != 0) {
        fprintf(stderr, "INSERT fehlgeschlagen: %s\n", mysql_error(conn));
        getch();
        return 1;
    }

    printf("\n" COLOR_GREEN "Neuer Datensatz wurde angelegt.\n" COLOR_RESET);
    printf(COLOR_CYANH "SQL: %s\n" COLOR_RESET, query);
    getch();
    return 0;
}

// Filterbedingungen abfragen und WHERE-Klausel bauen
void SetFilter(const char *tableName, char *filterBuffer, size_t bufferSize) {
    char q[256];
    snprintf(q, sizeof(q), "SELECT * FROM %s LIMIT 1;", tableName);

    if (mysql_query(conn, q) != 0) {
        getch();
        return;
    }
    MYSQL_RES *res = mysql_store_result(conn);
    if (!res) return;

    int num_fields = mysql_num_fields(res);
    MYSQL_FIELD *fields = mysql_fetch_fields(res);

    system("cls");
    printf(COLOR_CYAN "Filter setzen - Tabelle %s\n" COLOR_RESET, tableName);
    printf(COLOR_GRAY "Operatoren: >, <, >=, <=, =, !=\n");
    printf("Bereiche:   BETWEEN 10 AND 20  (oder Datum: 2025-01-01 AND 2025-02-01)\n");
    printf("Wildcards:  %% (z.B. 'Mueller%%')\n");
    printf("Leere Eingabe: Kein Filter.\n");
    printf("Wert 'NULL': Suche nach Feldern mit NULL.\n\n" COLOR_RESET);

    char currentCondition[4096] = "";
    int conditionsCount = 0;

    for (int i = 0; i < num_fields; i++) {

        // Prüfen, ob dieses Feld ein Fremdschlüssel ist
        char refTable[128];
        char refColumn[128];
        int isFK = getForeignKeyInfo(tableName, fields[i].name, refTable, sizeof(refTable), refColumn, sizeof(refColumn));

        // Prompt
        printf(COLOR_WHITE "%s" COLOR_RESET, fields[i].name);
        if (isFK) {
            printf(" (leer = kein Filter, ? = Auswahl, NULL = IS NULL)");
        } else {
            printf(" (leer = kein Filter, NULL = IS NULL)");
        }
        printf(": ");

        char input[512];
        readLine(input, sizeof(input));

        // Pointer auf ersten nicht-Leerzeichen-Char
        char *p = input;
        while (*p == ' ' || *p == '\t') p++;

        // Trimmed-Kopie für Spezialfälle ("?", "NULL")
        char trimmed[512];
        strncpy(trimmed, p, sizeof(trimmed) - 1);
        trimmed[sizeof(trimmed) - 1] = '\0';

        char *end = trimmed + strlen(trimmed);
        while (end > trimmed && (end[-1] == ' ' || end[-1] == '\t')) {
            *--end = '\0';
        }

        // Leere Eingabe -> kein Filter für dieses Feld
        if (trimmed[0] == '\0') {
            continue;
        }

        // FK + "?" -> Auswahl über InteractiveTable
        if (isFK && trimmed[0] == '?' && trimmed[1] == '\0') {

            printf(COLOR_GRAY "Fremdschluessel -> %s.%s, Auswahl ueber Tabelle, ESC = kein Filter\n" COLOR_RESET, refTable, refColumn);

            long selId = InteractiveTable(refTable, 1);
            if (selId > 0) {
                if (conditionsCount > 0) {
                    strcat(currentCondition, " AND ");
                }
                char cond[256];
                snprintf(cond, sizeof(cond), "%s = %ld", fields[i].name, selId);
                strcat(currentCondition, cond);
                conditionsCount++;
            }
            // ESC -> kein Filter
            continue;
        }

        // "NULL" (case-insensitive) -> IS NULL
        if ((trimmed[0] == 'N' || trimmed[0] == 'n') &&
            (trimmed[1] == 'U' || trimmed[1] == 'u') &&
            (trimmed[2] == 'L' || trimmed[2] == 'l') &&
            (trimmed[3] == 'L' || trimmed[3] == 'l') &&
            trimmed[4] == '\0')
        {
            if (conditionsCount > 0) {
                strcat(currentCondition, " AND ");
            }
            strcat(currentCondition, fields[i].name);
            strcat(currentCondition, " IS NULL");
            conditionsCount++;
            continue;
        }

        // Ab hier: normale Parserlogik (BETWEEN, Operatoren, LIKE ...)
        if (conditionsCount > 0) {
            strcat(currentCondition, " AND ");
        }

        strcat(currentCondition, fields[i].name);

        // BETWEEN?
        if (startsWithIgnoreCase(p, "BETWEEN")) {

            // nach 'BETWEEN' weiter
            char *startVal = p + 7;
            while (*startVal == ' ') startVal++;

            // " AND " oder " and " suchen
            char *sep = strstr(startVal, " AND ");
            if (!sep) sep = strstr(startVal, " and ");

            if (sep) {
                // Wert 1
                size_t len1 = (size_t)(sep - startVal);
                char val1[256];
                if (len1 >= sizeof(val1)) len1 = sizeof(val1) - 1;
                strncpy(val1, startVal, len1);
                val1[len1] = '\0';

                // Wert 2 (nach " AND ")
                char *val2 = sep + 5;
                while (*val2 == ' ') val2++;

                char esc1[512], esc2[512];
                mysql_real_escape_string(conn, esc1, val1, strlen(val1));
                mysql_real_escape_string(conn, esc2, val2, strlen(val2));

                strcat(currentCondition, " BETWEEN '");
                strcat(currentCondition, esc1);
                strcat(currentCondition, "' AND '");
                strcat(currentCondition, esc2);
                strcat(currentCondition, "'");
            } else {
                // User hat "BETWEEN ..." geschrieben, aber kein "AND" → roh übernehmen
                strcat(currentCondition, " ");
                strcat(currentCondition, p);
            }
        }
        else {
            // Normale Operatoren (>, <, =, !=, ...)
            char op[5] = "";
            if (strncmp(p, ">=", 2) == 0) { strcpy(op, ">="); p += 2; }
            else if (strncmp(p, "<=", 2) == 0) { strcpy(op, "<="); p += 2; }
            else if (strncmp(p, "!=", 2) == 0) { strcpy(op, "!="); p += 2; }
            else if (strncmp(p, "<>", 2) == 0) { strcpy(op, "<>"); p += 2; }
            else if (strncmp(p, ">", 1) == 0)  { strcpy(op, ">");  p += 1; }
            else if (strncmp(p, "<", 1) == 0)  { strcpy(op, "<");  p += 1; }
            else if (strncmp(p, "=", 1) == 0)  { strcpy(op, "=");  p += 1; }

            while (*p == ' ') p++;

            if (op[0] != '\0') {
                strcat(currentCondition, " ");
                strcat(currentCondition, op);
            } else {
                if (strchr(p, '%')) {
                    strcat(currentCondition, " LIKE");
                } else {
                    strcat(currentCondition, " =");
                }
            }

            // Wert immer escapen und in '...' setzen
            char esc[1024];
            mysql_real_escape_string(conn, esc, p, strlen(p));

            strcat(currentCondition, " '");
            strcat(currentCondition, esc);
            strcat(currentCondition, "'");
        }

        conditionsCount++;
    }

    mysql_free_result(res);

    if (conditionsCount > 0) {
        snprintf(filterBuffer, bufferSize, " WHERE %s", currentCondition);
        printf("\n" COLOR_GREEN "Filter gesetzt: %s\n" COLOR_RESET, filterBuffer);
    } else {
        filterBuffer[0] = '\0';
        printf("\n" COLOR_YELLOW "Filter geloescht.\n" COLOR_RESET);
    }

    printf("Weiter mit beliebiger Taste...");
    getch();
}


// Interaktive Tabellen-Schleife
// selectionMode: 1 = Enter gibt ID zurück, 0 = normale Verwaltung
long InteractiveTable(const char *tableName, int selectionMode) {
    int page = 0;
    const int pageSize = 20;
    int totalRows = 0;
    int selInPage = 0;
    char idField[128];
    char selectedId[128];
    int exitFlag = 0;
    long returnId = -1; // Standard: nichts gewählt

    MYSQL_RES *res = NULL;
    int needReload = 1;

    char filterClause[4096] = ""; 

    while (!exitFlag) {
        if (needReload) {
            if (res) {
                mysql_free_result(res);
                res = NULL;
            }

            char q[5120];
            snprintf(q, sizeof(q), "SELECT * FROM %s%s;", tableName, filterClause);

            if (mysql_query(conn, q) != 0) {
                fprintf(stderr, "Abfrage fehlgeschlagen: %s\n", mysql_error(conn));
                getch();
                return -1;
            }
            res = mysql_store_result(conn);
            if (!res) {
                fprintf(stderr, "Fehler beim Lesen des Ergebnisses: %s\n", mysql_error(conn));
                getch();
                return -1;
            }

            totalRows = (int)mysql_num_rows(res);

            if (page * pageSize >= totalRows) {
                page = 0;
                selInPage = 0;
            }

            int globalIndex = page * pageSize + selInPage;
            if (globalIndex >= totalRows) {
                if (totalRows <= 0) {
                    globalIndex = 0;
                    page = 0;
                    selInPage = 0;
                } else {
                    globalIndex = totalRows - 1;
                    page = globalIndex / pageSize;
                    selInPage = globalIndex % pageSize;
                }
            }
            needReload = 0;
        }

        if (!res) return -1;

        // Zeichnen mit Selection Mode Flag
        if (DrawInteractiveTable(res, tableName, page, pageSize, selInPage, &totalRows, idField, sizeof(idField),
                                 selectedId, sizeof(selectedId), selectionMode) != 0) {
            mysql_free_result(res);
            return -1;
        }

        if (filterClause[0] != '\0') {
            printf(COLOR_MAGENTA "Filter aktiv: %s\n" COLOR_RESET, filterClause);
        }

        int totalPages = (totalRows + pageSize - 1) / pageSize;
        if (totalPages == 0) totalPages = 1;
        
        int startIndex = page * pageSize;
        int rowsOnPage = totalRows - startIndex;
        if (rowsOnPage > pageSize) rowsOnPage = pageSize;
        if (rowsOnPage <= 0) selInPage = 0;
        else if (selInPage >= rowsOnPage) selInPage = rowsOnPage - 1;

        int ch = getch();
        if (ch == 0 || ch == 224) {
            ch = getch();
            if (ch == 72) {
                if (rowsOnPage > 0 && selInPage > 0) selInPage--;
            } else if (ch == 80) {
                if (rowsOnPage > 0 && selInPage < rowsOnPage - 1) selInPage++;
            } else if (ch == 75) {
                if (page > 0) {
                    page--;
                    selInPage = 0;
                }
            } else if (ch == 77) {
                if ((page + 1) * pageSize < totalRows) {
                    page++;
                    selInPage = 0;
                }
            }
        } else if (ch == 13 && selectionMode) { // ENTER im Auswahlmodus
            if (totalRows > 0 && selectedId[0] != '\0') {
                returnId = atol(selectedId);
                exitFlag = 1;
            }
        } else if (!selectionMode && (ch == 'w' || ch == 'W')) {
            if (totalRows > 0) {
                EditRecord(tableName, idField, selectedId);
                needReload = 1;
            }
        } else if (!selectionMode && (ch == 'a' || ch == 'A')) {
            AddRecord(tableName);
            needReload = 1;
        } else if (ch == 'f' || ch == 'F') {
            SetFilter(tableName, filterClause, sizeof(filterClause));
            needReload = 1;
            page = 0;
            selInPage = 0;
        } else if (ch == 27) {
            exitFlag = 1;
            returnId = -1;
        }
    }

    if (res) {
        mysql_free_result(res);
    }
    return returnId;
}

// Berichtsfunktion (nur Lesen)
int DrawReport(const char *title, const char *sqlQuery) {
    system("cls");
    printf(COLOR_CYAN "Bericht: %s\n" COLOR_GRAY "%s\n\n" COLOR_RESET, title, sqlQuery);


    if (mysql_query(conn, sqlQuery) != 0) {
        fprintf(stderr, "Abfrage fehlgeschlagen: %s\n", mysql_error(conn));
        getch();
        return 1;
    }
    MYSQL_RES *res = mysql_store_result(conn);
    if (!res) {
        fprintf(stderr, "Fehler beim Lesen des Ergebnisses: %s\n", mysql_error(conn));
        getch();
        return 1;
    }

    int num_fields = mysql_num_fields(res);
    MYSQL_FIELD *fields = mysql_fetch_fields(res);
    unsigned int col_widths[num_fields];

    for (int i = 0; i < num_fields; i++) {
        unsigned int header_len = utf8_strlen(fields[i].name);
        unsigned int data_len   = fields[i].max_length;
        col_widths[i] = (header_len > data_len ? header_len : data_len) + 2;
    }

    // Tabellenkopf
    printf("┌");
    for (int i = 0; i < num_fields; i++) {
        for (int j = 0; j <= (int)col_widths[i]; ++j) printf("─");
        if (i == num_fields - 1) printf("┐");
        else printf("┬");
    }
    printf("\n");

    printf("│ ");
    for (int i = 0; i < num_fields; i++) {
        printf("%-*s", (int)col_widths[i], fields[i].name);
        printf("│ ");
    }
    printf("\n");

    printf("├");
    for (int i = 0; i < num_fields; i++) {
        for (int j = 0; j <= (int)col_widths[i]; ++j) printf("─");
        if (i == num_fields - 1) printf("┤");
        else printf("┼");
    }
    printf("\n");

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        printf("│ ");
        for (int i = 0; i < num_fields; i++) {
            print_utf8_padded(row[i], col_widths[i]);
            printf("│ ");
        }
        printf("\n");
    }

    printf("└");
    for (int i = 0; i < num_fields; i++) {
        for (int j = 0; j <= (int)col_widths[i]; ++j) printf("─");
        if (i == num_fields - 1) printf("┘");
        else printf("┴");
    }
    printf("\n");

    mysql_free_result(res);
    printf(COLOR_GRAY "\nWeiter mit einer Taste...\n" COLOR_RESET);
    getch();
    return 0;
}

// Summe der Monatsbeiträge für einen Sportler berechnen
double CalculateBeitrag(long sportlerId) {
    char q[512];
    snprintf(q, sizeof(q), 
             "SELECT SUM(sa.monatsbeitrag) "
             "FROM sportler_sportart ss "
             "JOIN sportart sa ON sa.sportart_id = ss.sportart_id "
             "WHERE ss.sportler_id = %ld;", sportlerId);
    
    if (mysql_query(conn, q) != 0) return 0.0;
    
    MYSQL_RES *res = mysql_store_result(conn);
    if (!res) return 0.0;
    
    MYSQL_ROW row = mysql_fetch_row(res);
    double sum = 0.0;
    if (row && row[0]) {
        sum = atof(row[0]);
    }
    mysql_free_result(res);
    return sum;
}

// Name des Sportlers holen (für UI)
void GetSportlerName(long id, char *buffer, size_t size) {
    char q[256];
    snprintf(q, sizeof(q), "SELECT sportler_name FROM sportler WHERE sportler_id = %ld", id);
    if (mysql_query(conn, q) == 0) {
        MYSQL_RES *res = mysql_store_result(conn);
        if (res) {
            MYSQL_ROW row = mysql_fetch_row(res);
            if (row && row[0]) {
                strncpy(buffer, row[0], size - 1);
                buffer[size - 1] = '\0';
            }
            mysql_free_result(res);
        }
    }
}

// Berechnet den nächsten fälligen Monat für den Sportler
// Logik: 
// 1. Gibt es eine offene Rechnung (zahlungsdatum IS NULL)? -> Nimm den ältesten offenen Monat.
// 2. Sonst: Nimm den allerneuesten Monat + 1.
// 3. Wenn gar keine Daten: Nimm Beitrittsdatum.
void GetNextBillableMonth(long sportlerId, char *outBuffer, size_t size) {
    char q[1024];
    
    // Schritt 1: Prüfen auf offene Rechnungen
    snprintf(q, sizeof(q), 
             "SELECT DATE_FORMAT(MIN(fuer_monat), '%%Y-%%m') "
             "FROM zahlung WHERE sportler_id = %ld AND zahlungsdatum IS NULL;", 
             sportlerId);

    int foundOpenBill = 0;
    if (mysql_query(conn, q) == 0) {
        MYSQL_RES *res = mysql_store_result(conn);
        if (res) {
            MYSQL_ROW row = mysql_fetch_row(res);
            if (row && row[0]) {
                // Offene Rechnung gefunden! Das schlagen wir vor.
                strncpy(outBuffer, row[0], size - 1);
                outBuffer[size - 1] = '\0';
                foundOpenBill = 1;
            }
            mysql_free_result(res);
        }
    }

    // Schritt 2: Wenn keine offenen Rechnungen, dann neuer Monat (MAX + 1)
    if (!foundOpenBill) {
        snprintf(q, sizeof(q),
            "SELECT DATE_FORMAT("
            "  COALESCE("
            "    DATE_ADD(MAX(fuer_monat), INTERVAL 1 MONTH), "
            "    (SELECT beitrittsdatum FROM sportler WHERE sportler_id = %ld)"
            "  ), '%%Y-%%m') "
            "FROM zahlung WHERE sportler_id = %ld;", 
            sportlerId, sportlerId);

        if (mysql_query(conn, q) == 0) {
            MYSQL_RES *res = mysql_store_result(conn);
            if (res) {
                MYSQL_ROW row = mysql_fetch_row(res);
                if (row && row[0]) {
                    strncpy(outBuffer, row[0], size - 1);
                    outBuffer[size - 1] = '\0';
                } else {
                    // Fallback absolut: aktueller Monat
                     time_t t = time(NULL);
                     struct tm *tm = localtime(&t);
                     strftime(outBuffer, size, "%Y-%m", tm);
                }
                mysql_free_result(res);
            }
        }
    }
}

// Neue Zahlung erfassen (Komplettprozess)
void AddPaymentProcess(void) {
    // 1. Sportler auswählen
    long sportlerId = InteractiveTable("sportler", 1); 
    if (sportlerId == -1) return; 

    // Infos laden
    char sportlerName[100] = "Unbekannt";
    GetSportlerName(sportlerId, sportlerName, sizeof(sportlerName));

    // Vorschläge berechnen (jetzt mit korrigierter Logik)
    char suggestedMonth[20];
    GetNextBillableMonth(sportlerId, suggestedMonth, sizeof(suggestedMonth));
    
    double calcBeitrag = CalculateBeitrag(sportlerId);

    char todayStr[20];
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    strftime(todayStr, sizeof(todayStr), "%Y-%m-%d", tm);

    // Variablen für den Prozess
    char finalMonth[20];
    char finalDate[20];
    double finalBeitrag = 0.0;
    long existingZahlungId = -1; 
    int loop = 1;
    int isDateNull = 0; // Flag für NULL Datum

    system("cls");
    printf(COLOR_CYAN "Neue Zahlung erfassen\n" COLOR_RESET);
    printf("Sportler: " COLOR_WHITE "%s\n" COLOR_RESET, sportlerName);
    printf(COLOR_GRAY "(ESC zum Abbrechen bei jeder Eingabe)\n\n" COLOR_RESET);

    // --- SCHRITT 1: MONAT EINGEBEN ---
    while (loop) {
        printf("Monat (Format JJJJ-MM) [" COLOR_GREEN "%s" COLOR_RESET "]: ", suggestedMonth);
        
        char buf[64];
        if (!eingabeJahrMonat(buf, suggestedMonth)) {
            printf("\nAbbruch.\n");
            return;
        }
        printf("\n");

        strcpy(finalMonth, buf);

        // PRÜFUNG: Existiert Eintrag?
        char qCheck[512];
        snprintf(qCheck, sizeof(qCheck), 
                 "SELECT zahlung_id, zahlungsdatum FROM zahlung "
                 "WHERE sportler_id=%ld AND fuer_monat='%s-01'", sportlerId, finalMonth);
        
        if (mysql_query(conn, qCheck) == 0) {
            MYSQL_RES *res = mysql_store_result(conn);
            if (res) {
                MYSQL_ROW row = mysql_fetch_row(res);
                if (row) {
                    existingZahlungId = atol(row[0]);
                    char *payDate = row[1]; 

                    if (payDate && strlen(payDate) > 0) {
                        // Bereits bezahlt
                        printf(COLOR_MAGENTA "ACHTUNG: Dieser Monat wurde bereits am %s bezahlt!\n" COLOR_RESET, payDate);
                        printf("Bitte einen anderen Monat wählen.\n\n");
                        mysql_free_result(res);
                        continue; 
                    } else {
                        // Existiert, aber offen
                        printf(COLOR_YELLOW "Info: Offene Rechnung für diesen Monat gefunden. Wird bearbeitet.\n" COLOR_RESET);
                        loop = 0; 
                    }
                } else {
                    // Neu
                    existingZahlungId = -1;
                    loop = 0; 
                }
                mysql_free_result(res);
            }
        } else {
             printf(COLOR_RED "DB Fehler: %s\n" COLOR_RESET, mysql_error(conn));
             return;
        }
    }

    // --- SCHRITT 2: BEITRAG ---
    printf("Betrag [" COLOR_GREEN "%.2f" COLOR_RESET "]: ", calcBeitrag);
    char bufAmount[64];
    char defAmountStr[64];
    snprintf(defAmountStr, sizeof(defAmountStr), "%.2f", calcBeitrag);

    if (!eingabeText(bufAmount, sizeof(bufAmount), defAmountStr)) return;
    printf("\n");
    
    char *endptr;
    finalBeitrag = strtod(bufAmount, &endptr);
    if (endptr == bufAmount) finalBeitrag = calcBeitrag;

    // --- SCHRITT 3: ZAHLUNGSDATUM (mit NULL Support) ---
    printf("Zahlungsdatum (oder NULL) [" COLOR_GREEN "%s" COLOR_RESET "]: ", todayStr);
    char bufDate[64];
    if (!eingabeText(bufDate, sizeof(bufDate), todayStr)) return;
    printf("\n");
    
    // Prüfen auf "NULL" (case insensitive)
    if (startsWithIgnoreCase(bufDate, "NULL")) {
        isDateNull = 1;
        strcpy(finalDate, "NULL");
    } else {
        isDateNull = 0;
        // Validierungslänge entfernen, damit "NULL" nicht durch todayStr überschrieben wird
        if (strlen(bufDate) == 0) strcpy(finalDate, todayStr); // Sollte durch eingabeText Default schon passieren, aber zur Sicherheit
        else strcpy(finalDate, bufDate);
    }

    // --- SPEICHERN ---
    char q[2048];
    char dateValueSql[64];

    // SQL-String für das Datum vorbereiten (mit oder ohne Anführungszeichen)
    if (isDateNull) {
        strcpy(dateValueSql, "NULL");
    } else {
        snprintf(dateValueSql, sizeof(dateValueSql), "'%s'", finalDate);
    }

    if (existingZahlungId != -1) {
        // UPDATE
        snprintf(q, sizeof(q),
                 "UPDATE zahlung SET zahlungsdatum=%s, beitrag=%.2f "
                 "WHERE zahlung_id=%ld",
                 dateValueSql, finalBeitrag, existingZahlungId);
    } else {
        // INSERT
        snprintf(q, sizeof(q),
                 "INSERT INTO zahlung (sportler_id, beitrag, fuer_monat, zahlungsdatum) "
                 "VALUES (%ld, %.2f, '%s-01', %s)",
                 sportlerId, finalBeitrag, finalMonth, dateValueSql);
    }

    if (mysql_query(conn, q) != 0) {
        printf(COLOR_RED "\nFehler beim Speichern: %s\n" COLOR_RESET, mysql_error(conn));
    } else {
        printf(COLOR_GREEN "\nErfolg! Datensatz wurde %s.\n" COLOR_RESET, 
               (existingZahlungId != -1) ? "aktualisiert" : "angelegt");
    }
    
    printf("Taste drücken...");
    getch();
}

// Neuer Verkaufsprozess für Schema mit kunden / mitarbeiter / artikels / bestellungen / bestellpositionen
void Verkaufen(void) {
    typedef struct {
        long  artikelId;
        int   menge;
        double preis;
    } OrderItem;

    OrderItem items[100];
    int itemCount = 0;

    system("cls");
    printf(COLOR_CYAN "Neuen Verkauf erfassen\n" COLOR_RESET);
    printf(COLOR_GRAY "(ESC im Auswahlfenster bricht den Vorgang ab)\n\n" COLOR_RESET);

    // 1) Kunde auswählen
    printf("Bitte Kunden waehlen...\n");
    long kundeId = InteractiveTable("kunden", 1);
    if (kundeId <= 0) {
        printf(COLOR_YELLOW "Kein Kunde gewaehlt. Vorgang abgebrochen.\n" COLOR_RESET);
        getch();
        return;
    }

    // 2) Mitarbeiter auswählen
    printf("\nBitte Mitarbeiter waehlen...\n");
    long mitarbeiterId = InteractiveTable("mitarbeiter", 1);
    if (mitarbeiterId <= 0) {
        printf(COLOR_YELLOW "Kein Mitarbeiter gewaehlt. Vorgang abgebrochen.\n" COLOR_RESET);
        getch();
        return;
    }

    // 3) Artikel in einer Schleife hinzufügen
    while (1) {
        printf("\nArtikel fuer die Bestellung waehlen...\n");
        long artikelId = InteractiveTable("artikels", 1);

        if (artikelId <= 0) {
            if (itemCount == 0) {
                printf(COLOR_YELLOW "Keine Artikel ausgewaehlt. Vorgang abgebrochen.\n" COLOR_RESET);
                getch();
                return;
            } else {
                // keine weitere Position
                break;
            }
        }

        // Lagerbestand und Preis laden
        char q[512];
        snprintf(q, sizeof(q),
                 "SELECT preis, menge FROM artikels WHERE artikel_id=%ld;", artikelId);

        if (mysql_query(conn, q) != 0) {
            printf(COLOR_RED "Fehler beim Lesen des Artikels: %s\n" COLOR_RESET, mysql_error(conn));
            getch();
            return;
        }

        MYSQL_RES *res = mysql_store_result(conn);
        if (!res) {
            printf(COLOR_RED "Fehler beim Lesen des Ergebnisses: %s\n" COLOR_RESET, mysql_error(conn));
            getch();
            return;
        }

        MYSQL_ROW row = mysql_fetch_row(res);
        if (!row) {
            mysql_free_result(res);
            printf(COLOR_RED "Artikel nicht gefunden.\n" COLOR_RESET);
            getch();
            return;
        }

        double preis = row[0] ? atof(row[0]) : 0.0;
        long lagerMenge = row[1] ? atol(row[1]) : 0;

        mysql_free_result(res);

        if (lagerMenge <= 0) {
            printf(COLOR_RED "Dieser Artikel ist nicht auf Lager (Menge = 0).\n" COLOR_RESET);
            printf("Bitte anderen Artikel waehlen.\n");
            continue;
        }

        printf("Artikel-ID: %ld, Preis: %.2f, Lager: %ld\n", artikelId, preis, lagerMenge);

        // Menge abfragen und direkt gegen Lager pruefen
        int bestellMenge = 0;
        while (1) {
            char buf[64];
            printf("Menge (1..%ld) [1]: ", lagerMenge);

            if (!eingabeText(buf, sizeof(buf), "1")) {
                // ESC -> aktuelle Artikelwahl abbrechen
                printf("\nArtikel wird nicht hinzugefuegt.\n");
                bestellMenge = 0;
                break;
            }
            printf("\n");

            long m = atol(buf);
            if (m <= 0) {
                printf(COLOR_RED "Menge muss groesser als 0 sein.\n" COLOR_RESET);
                continue;
            }
            if (m > lagerMenge) {
                printf(COLOR_RED "Nicht genug auf Lager. Maximal verfuegbar: %ld\n" COLOR_RESET, lagerMenge);
                continue;
            }
            bestellMenge = (int)m;
            break;
        }

        if (bestellMenge <= 0) {
            // Artikel wird nicht aufgenommen, aber Schleife kann weiterlaufen
            continue;
        }

        if (itemCount >= 100) {
            printf(COLOR_YELLOW "Maximale Anzahl von 100 Positionen erreicht.\n" COLOR_RESET);
            break;
        }

        items[itemCount].artikelId = artikelId;
        items[itemCount].menge     = bestellMenge;
        items[itemCount].preis     = preis;
        itemCount++;

        printf(COLOR_GREEN "Artikel hinzugefuegt: ID %ld, Menge %d, Einzelpreis %.2f\n" COLOR_RESET,
               artikelId, bestellMenge, preis);

        // Noch eine Position?
        printf("Noch einen Artikel hinzufuegen? (j/n): ");
        int ch;
        do {
            ch = getch();
        } while (ch != 'j' && ch != 'J' && ch != 'n' && ch != 'N');
        printf("%c\n", ch);
        if (ch != 'j' && ch != 'J') break;
    }

    if (itemCount == 0) {
        printf(COLOR_YELLOW "Keine Artikel in der Bestellung. Vorgang abgebrochen.\n" COLOR_RESET);
        getch();
        return;
    }

    // 4) SQL-Befehle bauen und ausführen
    char sqlLog[8192];
    sqlLog[0] = '\0';

    // Datum = heute
    time_t t = time(NULL);
    struct tm *tmv = localtime(&t);
    char today[11];
    strftime(today, sizeof(today), "%Y-%m-%d", tmv);

    // Transaktion starten (optional, aber schoen fuer Konsistenz)
    if (mysql_query(conn, "START TRANSACTION;") != 0) {
        printf(COLOR_RED "Fehler bei START TRANSACTION: %s\n" COLOR_RESET, mysql_error(conn));
        getch();
        return;
    }
    strncat(sqlLog, "START TRANSACTION;", sizeof(sqlLog) - strlen(sqlLog) - 1);
    strncat(sqlLog, "\n", sizeof(sqlLog) - strlen(sqlLog) - 1);

    // Bestellung anlegen
    char q[1024];
    snprintf(q, sizeof(q),
             "INSERT INTO bestellungen (kunde_id, datum, status, mitarbeiter_id) "
             "VALUES (%ld, '%s', 'offen', %ld);",
             kundeId, today, mitarbeiterId);

    if (mysql_query(conn, q) != 0) {
        printf(COLOR_RED "Fehler beim Anlegen der Bestellung: %s\n" COLOR_RESET, mysql_error(conn));
        mysql_query(conn, "ROLLBACK;");
        getch();
        return;
    }
    strncat(sqlLog, q, sizeof(sqlLog) - strlen(sqlLog) - 1);
    strncat(sqlLog, "\n", sizeof(sqlLog) - strlen(sqlLog) - 1);

    long bestId = (long)mysql_insert_id(conn);

    // Positionen + Lagerbestand aktualisieren
    for (int i = 0; i < itemCount; i++) {
        // Position
        snprintf(q, sizeof(q),
                 "INSERT INTO bestellpositionen (best_id, artikel_id, menge, price) "
                 "VALUES (%ld, %ld, %d, %.2f);",
                 bestId,
                 items[i].artikelId,
                 items[i].menge,
                 items[i].preis);

        if (mysql_query(conn, q) != 0) {
            printf(COLOR_RED "Fehler beim Anlegen der Position: %s\n" COLOR_RESET, mysql_error(conn));
            mysql_query(conn, "ROLLBACK;");
            getch();
            return;
        }
        strncat(sqlLog, q, sizeof(sqlLog) - strlen(sqlLog) - 1);
        strncat(sqlLog, "\n", sizeof(sqlLog) - strlen(sqlLog) - 1);

        // Lagerbestand verringern
        snprintf(q, sizeof(q),
                 "UPDATE artikels SET menge = menge - %d WHERE artikel_id = %ld;",
                 items[i].menge,
                 items[i].artikelId);

        if (mysql_query(conn, q) != 0) {
            printf(COLOR_RED "Fehler beim Aktualisieren des Lagerbestands: %s\n" COLOR_RESET, mysql_error(conn));
            mysql_query(conn, "ROLLBACK;");
            getch();
            return;
        }
        strncat(sqlLog, q, sizeof(sqlLog) - strlen(sqlLog) - 1);
        strncat(sqlLog, "\n", sizeof(sqlLog) - strlen(sqlLog) - 1);
    }

    // Commit
    if (mysql_query(conn, "COMMIT;") != 0) {
        printf(COLOR_RED "Fehler bei COMMIT: %s\n" COLOR_RESET, mysql_error(conn));
        mysql_query(conn, "ROLLBACK;");
        getch();
        return;
    }
    strncat(sqlLog, "COMMIT;", sizeof(sqlLog) - strlen(sqlLog) - 1);
    strncat(sqlLog, "\n", sizeof(sqlLog) - strlen(sqlLog) - 1);

    // 5) Ergebnis anzeigen
    system("cls");
    printf(COLOR_GREEN "Bestellung wurde erfolgreich angelegt. Bestell-ID: %ld\n\n" COLOR_RESET, bestId);
    printf(COLOR_CYANH "Ausgefuehrte SQL-Befehle:\n%s" COLOR_RESET, sqlLog);
    printf("\n\nWeiter mit beliebiger Taste...");
    getch();
}


int checkTableExists(const char *tableName) {
    char q[512];
    char esc[256];
    mysql_real_escape_string(conn, esc, tableName, strlen(tableName));
    snprintf(q, sizeof(q), "SHOW TABLES LIKE '%s';", esc);
    
    if (mysql_query(conn, q) != 0) return 0;
    MYSQL_RES *res = mysql_store_result(conn);
    if (!res) return 0;
    
    int exists = (mysql_num_rows(res) > 0);
    mysql_free_result(res);
    return exists;
}

int ParseConfigFile(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        fprintf(stderr, COLOR_RED "FEHLER: Konfigurationsdatei '%s' nicht gefunden.\n" COLOR_RESET, filename);
        return 0;
    }

    char line[4096];
    while (fgets(line, sizeof(line), file)) {
        trim_newline(line);
        if (line[0] == '#' || line[0] == '\0') continue; //comment

        char *eq = strchr(line, '='); //Suchen für Positions des Symbols "="
        if (eq) {
            *eq = '\0';
            char *key = line;
            char *val = eq + 1;

            if (strcmp(key, "HOST") == 0) strcpy(server, val);
            else if (strcmp(key, "USER") == 0) strcpy(user, val);
            else if (strcmp(key, "PASS") == 0) strcpy(password, val);
            else if (strcmp(key, "DB") == 0) strcpy(database, val);
            else if (strcmp(key, "TABLE") == 0) {
                AddConfigNode("TABLE", val, NULL);
            } else if (strcmp(key, "REPORT") == 0) {
                char *sep = strchr(val, ':'); //Suchen für Positions des Symbols ":"
                if (sep) {
                    *sep = '\0';
                    AddConfigNode("REPORT", val, sep + 1); //key=Titel; val=SQL
                }
            } else if (strcmp(key, "FUNC") == 0) {
                char *sep = strchr(val, ':');
                if (sep) {
                    *sep = '\0';
                    AddConfigNode("FUNC", val, sep + 1); // key=Titel
                }
            }
        }
    }
    fclose(file);
    return 1; // OK
}

int main(int argc, char *argv[])
{
    SetConsoleOutputCP(CP_UTF8);

    unsigned int curMenuItem = 0;
    const unsigned int countMenuItems = 13;

    const char *config_filename = "db.conf";
    //Konfigurationsdatei parameter checken (-conf:DATEINAME)
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "-conf:", 6) == 0) {
            config_filename = argv[i] + 6;
        }
    }

    if (!ParseConfigFile(config_filename)) {
        return 1;
    }

    conn = mysql_init(NULL);
    if (!conn) {
        fprintf(stderr, "mysql_init() fehlgeschlagen\n");
        return 1;
    }

    if (!mysql_real_connect(conn, server, user, password, database, 3306, NULL, 0)) {
        fprintf(stderr, "Verbindung fehlgeschlagen: %s\n", mysql_error(conn));
        mysql_close(conn);
        return 1;
    }

    if (mysql_set_character_set(conn, "utf8")) {
        fprintf(stderr, "Fehler beim Setzen des Zeichensatzes utf8: %s\n", mysql_error(conn));
    }

    //
    ConfigNode *curr = g_configList;
    while (curr) {
        if (strcmp(curr->type, "TABLE") == 0) {
            char displayTitle[200];
            snprintf(displayTitle, sizeof(displayTitle), "TABELLE: %s", curr->key);
            if (checkTableExists(curr->key)) {
                AddMenuItem(displayTitle, cb_Table, curr->key);
            } else {
                printf(COLOR_RED "Warnung: Tabelle '%s' nicht in Datenbank gefunden.\n" COLOR_RESET, curr->key);
            }
        }
        else if (strcmp(curr->type, "REPORT") == 0) {
            char displayTitle[200];
            snprintf(displayTitle, sizeof(displayTitle), "BERICHT: %s", curr->key);
            char contextBuf[4096];
            snprintf(contextBuf, sizeof(contextBuf), "%s:%s", curr->key, curr->value);
            AddMenuItem(displayTitle, cb_Report, contextBuf);
        }
        else if (strcmp(curr->type, "FUNC") == 0) {
            // Mapping von Text-Code auf echte C-Funktion
            if (strcmp(curr->value, "Payment") == 0) {
                AddMenuItem(curr->key, cb_Payment, NULL);
            } else if (strcmp(curr->value, "Verkaufen") == 0) {
                AddMenuItem(curr->key, cb_Verkaufen, NULL);
            }
        }
        curr = curr->next;
    }

    AddMenuItem("Das Programm beenden", cb_Exit, NULL);

    DrawMainMenu(curMenuItem);
    int ch;
    do {
        ch = getch();
        if (ch == 13) { 
            if (curMenuItem == g_menuCount - 1) {
                break; // Exit
            }
            if (g_menuItems[curMenuItem].callback) {
                g_menuItems[curMenuItem].callback(g_menuItems[curMenuItem].context);
            }
            DrawMainMenu(curMenuItem);
            
        } else if (ch == 0 || ch == 224) {
            ch = getch();
            if (ch == 72) { 
                if (curMenuItem > 0) curMenuItem--;
                DrawMainMenu(curMenuItem);
            } else if (ch == 80) { 
                if (curMenuItem < g_menuCount - 1) curMenuItem++;
                DrawMainMenu(curMenuItem);
            }
        }
    } while (ch != 3); // Strg+C

    mysql_close(conn);
    return 0;
}
