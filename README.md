# mysql-uni
Umschulung Projekt in C zur universellen Arbeit mit MySQL Database

## 1. Architektur-Überblick

Diese Anwendung ist ein Konsolen-Client für MySQL-Datenbanken, geschrieben in C. Die
Kernphilosophie ist ein Data-Driven UI (datengetriebene Benutzeroberfläche). Das bedeutet: Die
Struktur des Hauptmenüs und die verfügbaren Funktionen sind nicht fest im Code verdrahtet, sondern
werden beim Start vollständig über eine Konfigurationsdatei (.conf) definiert.

Technisch nutzt das Programm die native C-API (libmysqlclient) für die Datenbankkommunikation
und die Windows Console API für die Darstellung (Farben, Cursor-Positionierung).

## 2. Initialisierung & Konfiguration (.conf)

Der Einstiegspunkt ist die Funktion main in mysql-uni.c. Das Programm akzeptiert ein
Kommandozeilenargument **-conf:DATEINAME** (Standard ist **db.conf**).

**Konfiguration parsen (*ParseConfigFile*)**

Die Config-Datei wird Zeile für Zeile eingelesen. Es kommt ein simpler "Schlüssel=Wert"-Parser zum
Einsatz. Die eingelesenen Daten landen in einer einfach verketteten Liste g_configList (Struct
ConfigNode).

**Unterstützte Befehle:**

- HOST, USER, PASS, DB: Zugangsdaten für MySQL.
- TABLE: Name einer Tabelle, für die automatisch ein CRUD-Interface (Erstellen, Lesen, Aktualisieren, Löschen) generiert wird.
- REPORT: Format Titel:SQL_Query. Erstellt einen Menüpunkt für einen schreibgeschützten Bericht.
- FUNC: Format Titel:Interner_Name. Verknüpft einen Menüpunkt mit spezifischer Business-Logik im Code.

**Aufbau des Hauptmenüs**

Sobald die DB-Verbindung steht, iteriert das Programm über g_configList und füllt das Array
g_menuItems:

1. TABLE: Prüft vorab, ob die Tabelle existiert (checkTableExists). Falls ja, wird der Callback
    cb_Table registriert.
2. REPORT: Registriert den Callback cb_Report. Der SQL-Query wird im Context-Feld gespeichert.
3. FUNC: Mappt den Namen aus der Config auf fest definierte C-Funktionen (siehe Abschnitt
    "Geplante Änderungen").

## 3. UI-Core: InteractiveTable

Die Funktion InteractiveTable ist das Herzstück der Anwendung. Sie stellt eine universelle
Oberfläche bereit, um beliebige Tabellen anzuzeigen und zu bearbeiten.


Signatur:

```
long InteractiveTable(const char *tableName, int selectionMode);
```
Funktionsweise

1. **Lazy Loading & Paging:**
    Es werden nicht alle Daten sofort in den Speicher geladen, aber der MYSQL_RES Zeiger bleibt offen.
    Die Darstellung erfolgt seitenweise (pageSize = 20). Es wird immer nur das aktuelle "Fenster" gerendert.
    Bei der Navigation (Pfeiltasten) ändert sich lediglich der Index (selInPage) oder die Seite (page).
2. **Betriebsmodi (selectionMode):**
    - Normal (0). Standard-Modus zum Ansehen, Bearbeiten (w), Hinzufügen (a) und Filtern (f). Rückgabewert beim Beenden ist -1.
    - Auswahl (1): Wird genutzt, um einen Fremdschlüssel auszuwählen. Drückt der User ENTER, wird die ID des markierten Datensatzes zurückgegeben.
3. **Behandlung von Fremdschlüsseln (Foreign Keys):**
    Beim Bearbeiten oder Erstellen eines Datensatzes (inputValueForField) prüft das Programm via INFORMATION_SCHEMA, ob ein Feld ein Fremdschlüssel ist.
    Ist das der Fall, wird InteractiveTable(RefTable, 1) rekursiv aufgerufen.
    Der User sieht dann die verknüpfte Tabelle, wählt einen Eintrag aus, und die ID wird
    automatisch in das Eingabefeld übernommen.
4. **Filterung (*SetFilter*):**
    Der User kann Bedingungen eingeben (z.B. LIKE, =, >, BETWEEN).
    Daraus wird eine WHERE-Klausel generiert, das MYSQL_RES neu geladen und die Paginierung
    zurückgesetzt.

## 4. Berichtswesen: DrawReport

Die Funktion *DrawReport* dient der Anzeige von Ergebnissen beliebiger SQL-Abfragen, die in der Config definiert wurden.

Besonderheiten:

**Dynamisches Layout:** Die Spaltenbreiten werden automatisch berechnet. Basis sind die Metadaten (*mysql_fetch_fields*) und die tatsächliche Länge der Daten (*utf8_strlen*), damit die Tabelle auch bei Umlauten sauber ausgerichtet bleibt.
**Read-Only**: Im Gegensatz zu *InteractiveTable* gibt es hier keinen Auswahl-Cursor und keine Bearbeitungsfunktionen.

## 5. Eigene Funktionen (FUNC) und Plugins


Aktuelle Implementierung (Legacy/Workaround)

Momentan wird der FUNC-Befehl aus der Config durch einfache String-Vergleiche (strcmp) in der
main-Funktion verarbeitet:

```
// mysql-uni.c
else if (strcmp(curr->type, "FUNC") == 0) {
if (strcmp(curr->value, "Payment") == 0) {
AddMenuItem(curr->key, cb_Payment, NULL);
}
else if (strcmp(curr->value, "Verkaufen") == 0) { ... }
// ...
}
```
Das ist unschön, da für jede neue Business-Logik (z.B. Verkaufen oder RechnungDrucken) der Core
(mysql-uni.exe) neu kompiliert werden muss.

**Geplante Architektur (DLL Plugins)**

Es ist ein Refactoring hin zu einem echten Plugin-System geplant:

1. Der FUNC-Eintrag in der Config soll auf eine DLL und den Funktionsnamen verweisen.
    Beispiel: FUNC=Ware verkaufen:plugins/shop.dll:Verkaufen
2. Der Kern würde dann LoadLibrary und GetProcAddress (Windows API) nutzen, um den Code
    dynamisch zur Laufzeit zu laden.
3. Das ermöglicht es, neue Logik (wie AddPaymentProcess) in separaten Projekten zu entwickeln,
    ohne den Quellcode von mysql-uni.c anfassen zu müssen.

## 6. Hilfsmodule

*myfunc.h*: Enthält Low-Level-Funktionen für die Konsole.
- *eingabeText* / *eingabeJahrMonat*: Sichere Eingabefunktionen mit ESC-Support (Abbruch).
- *utf8_strlen*: Korrekte Berechnung der String-Länge für die Tabellenausrichtung (essenziell für korrekte Darstellung von Umlauten).