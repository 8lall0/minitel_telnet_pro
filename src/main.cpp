#include <Arduino.h>
#include <Minitel1B_Hard.h>
#include <Preferences.h>
#include <WiFi.h>
#include <FS.h>
#include <SPIFFS.h>
#include <HardwareSerial.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include "sshClient.h"

#define MINITEL_BAUD_TRY  4800

#define WIFI_TIMEOUT 10000
#define WIFI_BEGIN 1
#define WIFI_WAITING 2
#define WIFI_ABORTED 3
#define WIFI_CONNECTED 4

#define HTTP_SERVER_HEADER_MAX_LENGTH 512
#define HTTP_SERVER_BODY_MAX_LENGTH   4096 // support for 4096-bit RSA private keys

#define HTTP_SERVER_READY            2
#define HTTP_SERVER_CLIENT_CONNECTED 3
#define HTTP_SERVER_HEADER_OVERSIZED 4
#define HTTP_SERVER_BODY_OVERSIZED   5
#define HTTP_SERVER_BODY_COMPLETE    6
#define HTTP_SERVER_GET_RECEIVED     7
#define HTTP_SERVER_UNKNOWN_METHOD   8
#define HTTP_SERVER_CLOSED           9

void reset();
void modeVideotex();
void modeMixte();
void extendedKeyboard();
void loadPrefs();
void readPresets();
void showPrefs();
int setPrefs();
void separateUrl(String urlToSeparate);
int setParameter(int x, int y, String& destination, bool mask, bool allowBlank, int (*httpHandler)());
void setIntParameter(int x, int y, uint16_t& destination);
void clearLineFromCursor();
void showHelp();
void sshTask(void* pvParameters);
void webSocketEvent(WStype_t type, uint8_t* payload, size_t len);
void writeBool(bool value);
void teletelMode();
void switchParameter(int x, int y, bool& destination);
void loopTelnet();
void loopWebsocket();
void loopSsh();
void loopSerial();
void printStringValue(const String& s);
void savePresets();
void prestelMode();
void cycleConnectionType(int x, int y);
void loadPresets();
void printPassword(const String& pwd);
void displayPresets(const String& title);
void writeConnectionType(byte connType);
String inputString(const String& defaultValue, int& exitCode, char padChar, int (*httpHandler)());
unsigned int numberOfChars(const String& str);
int manageHttpConnection();
void writePresets();

HardwareSerial& minitelPort = Serial2;
HardwareSerial& debugPort = Serial;
Minitel minitel(minitelPort);

WiFiClient telnet;
Preferences prefs;
WebSocketsClient webSocket;
SSHClient sshClient;
TaskHandle_t sshTaskHandle;

// WiFi credentials
String ssid("");
String password("");

bool advanced = false; // false=Minitel1, true=Minitel1B or above
bool functionKey = false; // flag use when reading keyboard byte by byte
String url("");
String host("");
String path("");
uint16_t port = 0;
bool scroll = true;
bool echo = false;
bool col80 = false;
bool prestel = false;
bool altcharset = false;
bool privKey = false;
int ping_ms = 0;
String protocol("");
String sshUser("");
String sshPass("");
String sshPrivKey("");

byte connectionType = 0; // 0=Telnet 1=Websocket 2=SSH 3=Serial
bool ssl = false;


typedef struct Preset_s
{
    String presetName = "";
    String url = "";
    bool scroll = false;
    bool echo = false;
    bool col80 = false;
    bool prestel = false;
    bool altcharset = false;
    bool privKey = false;
    byte connectionType = 0;
    int ping_ms = 0;
    String protocol = "";
    String sshUser = "";
    String sshPass = "";
    String sshPrivKey = "";
} Preset;

Preset presets[20];
int speed;
uint8_t wifiStatus;

WiFiServer server(80);
uint8_t serverStatus = HTTP_SERVER_CLOSED;

void initFS()
{
    boolean ok = SPIFFS.begin();
    if (!ok)
    {
        minitel.attributs(CLIGNOTEMENT);
        minitel.println("Formating SPIFFS, please wait");
        minitel.attributs(FIXE);
        ok = SPIFFS.format();
        if (ok) SPIFFS.begin();
    }
    if (!ok)
    {
        debugPort.printf("%% Aborting now. Problem initializing Filesystem. System HALTED\n");
        minitel.println("System HALTED.");
        minitel.println("problem initializing filesystem");
        while (true)
        {
            delay(5000);
        }
    }
    debugPort.printf("%% Mounted SPIFFS used=%d total=%d\r\n", SPIFFS.usedBytes(), SPIFFS.totalBytes());
}

// ESP_RST_POWERON = 1 = pressed hardware reset button
// ESP_RST_SW      = 3 = called ESP.restart() function
// ESP_RST_PANIC   = 4 = after raising exception (e.g. segmentation fault)
esp_reset_reason_t reset_reason;

void setup()
{
    reset_reason = esp_reset_reason();
    debugPort.begin(115200);
    debugPort.println("----------------");
    debugPort.println("Debug ready");
    debugPort.printf("RESET_REASON = %d\n", reset_reason);

    // Minitel setup
    // (don't) teletelMode();
    speed = MINITEL_BAUD_TRY;
    minitelPort.updateBaudRate(speed); // override minitel1b_Hard default speed
    if (speed != minitel.currentSpeed())
    {
        // avoid unwanted characters when restarting
        if ((speed = minitel.searchSpeed()) < MINITEL_BAUD_TRY)
        {
            // search speed
            if ((speed = minitel.changeSpeed(MINITEL_BAUD_TRY)) < 0)
            {
                // set to MINITEL_BAUD_TRY if different
                speed = minitel.searchSpeed(); // search speed again if change has failed
            }
        }
    }
    advanced = speed > 1200;

    // minitel.changeSpeed(speed);
    debugPort.printf("Minitel baud set to %d\n", speed);

    bool connectionOk = true;
    do
    {
        modeVideotex();
        minitel.writeByte(0x1b);
        minitel.writeByte(0x28);
        minitel.writeByte(0x40); // Standard G0 textmode charset
        minitel.writeByte(0x1b);
        minitel.writeByte(0x50); // Set black background
        minitel.attributs(FIXE);
        minitel.attributs(DEMASQUAGE);
        minitel.textMode();
        minitel.newXY(1, 1);
        extendedKeyboard();
        minitel.textMode();
        minitel.newScreen();
        minitel.echo(false);
        minitel.pageMode();

        loadPrefs();
        debugPort.println("Prefs loaded");

        readPresets();
        debugPort.println("Presets loaded");

        showPrefs();
        setPrefs();

        // Wi-Fi connection
        if (connectionType != 3)
        {
            // Wi-Fi not needed for serial

            separateUrl(url);

            minitel.capitalMode();
            minitel.println("Connecting, please wait.");
        }


        if (connectionType == 0)
        {
            // TELNET --------------------------------------------------------------------------------------
            // Telnet server connection
            delay(100);
            debugPort.printf("Connecting to %s\n", host.c_str());
            minitel.print("Connecting to ");
            minitel.print(host);
            minitel.print(":");
            minitel.println(String(port));

            if (telnet.connect(host.c_str(), port))
            {
                debugPort.println("Connected");
            }
            else
            {
                debugPort.println("Connection failed");
                minitel.println();
                minitel.println("Connection Refused. Press any key");
                while (minitel.getKeyCode() == 0)
                {
                }
                connectionOk = false;
            }
        }
        else if (connectionType == 1)
        {
            // WEBSOCKET -----------------------------------------------------------------------------
            debugPort.printf("ssl=%d, host=%s, port=%d, path=%s, protocol='%s'\n", ssl, host.c_str(), port,
                             path.c_str(),
                             protocol.c_str());

            if (protocol == "")
            {
                if (ssl) webSocket.beginSSL(host.c_str(), port, path.c_str());
                else webSocket.begin(host.c_str(), port, path.c_str());
            }
            else
            {
                debugPort.printf("  - subprotocol added\n");
                if (ssl) webSocket.beginSSL(host.c_str(), port, path.c_str(), protocol.c_str());
                else webSocket.begin(host.c_str(), port, path.c_str(), protocol.c_str());
            }

            webSocket.onEvent(webSocketEvent);

            if (ping_ms != 0)
            {
                debugPort.printf("  - heartbeat ping added\n");
                // start heartbeat (optional)
                // ping server every ping_ms
                // expect pong from server within 3000 ms
                // consider connection disconnected if pong is not received 2 times
                webSocket.enableHeartbeat(ping_ms, 3000, 2);
            }
        }
        else if (connectionType == 2)
        {
            // SSH ---------------------------------------------------------------------------------------
            debugPort.printf("\n> SSH task setup\n");
            BaseType_t xReturned = xTaskCreatePinnedToCore(sshTask, "sshTask", 51200, nullptr,
                                                           (configMAX_PRIORITIES - 1), &sshTaskHandle,
                                                           ARDUINO_RUNNING_CORE);
            if (xReturned != pdPASS)
            {
                debugPort.printf("  > Failed to create task\n");
            }
        }
        else if (connectionType == 3)
        {
            debugPort.begin(speed, SERIAL_7E1);
            debugPort.println();
            debugPort.println("Minitel serial port setup");

            minitel.capitalMode();
            minitel.attributs(DOUBLE_HAUTEUR);
            minitel.print("Minitel to Usb Serial adapter");
            minitel.newXY(1, 4);
            minitel.println(" PORT SETTINGS");
            minitel.println();
            minitel.print("  * Baud rate: ");
            minitel.println(String(speed));
            minitel.println("  * Data bits: 7");
            minitel.println("  * Parity   : E");
            minitel.println("  * Stop bit : 1");
            minitel.println();
            minitel.println();
            minitel.println(" Ctrl+R to restart");
            delay(advanced ? 2000 : 3000); // ok to use as no Wi-Fi is involved here
            minitel.cursor();
        } // --------------------------------------------------------------------------------------------------------------------------
    }
    while (!connectionOk);

    minitel.textMode();
    minitel.newXY(1, 1);
    minitel.newScreen();

    if (!prestel || col80)
    {
        // Set 40 or 80 columns
        if (col80)
        {
            modeMixte();
            minitel.writeByte(altcharset ? 0x0e : 0x0f); // US ASCII Charset in 80 columns
        }
        else
        {
            modeVideotex();
            minitel.textMode();
        }

        // Set echo
        minitel.echo(echo);

        // Set scroll
        if (scroll)
        {
            minitel.scrollMode();
        }
        else
        {
            minitel.pageMode();
        }

        minitel.newXY(1, 1);
        minitel.newScreen();
    }
    else
    {
        // prestel = true;
        minitel.changeSpeed(1200); // decrease speed
        prestelMode();
        // ECHO PRESTEL ON/OFF
    }

    if (connectionType != 3)
    {
        // debug port used for serial
        debugPort.println("Minitel initialized");
    }
}

void loop()
{
    if (connectionType == 0) // TELNET
        loopTelnet();
    else if (connectionType == 1) // WEBSOCKET
        loopWebsocket();
    else if (connectionType == 2) // SSH
        loopSsh();
    else if (connectionType == 3) // SERIAL
        loopSerial();
}

void loopTelnet()
{
    if (telnet.available())
    {
        const int tmp = telnet.read();
        minitel.writeByte(static_cast<byte>(tmp));
        debugPort.printf("[telnet] 0x%X\n", tmp);
    }

    if (minitelPort.available() > 0)
    {
        const byte tmp = minitel.readByte();
        if (tmp == 18 || (functionKey && tmp == 0x49))
        {
            // CTRL+R = RESET ou TS+CONNEXION
            telnet.stop();

            if (!col80 && prestel)
            {
                teletelMode();
            }
            modeVideotex();
            minitel.newXY(1, 1);
            minitel.newScreen();
            minitel.echo(true);
            minitel.pageMode();
            reset();
        }
        functionKey = (tmp == 0x13);
        telnet.write((uint8_t)tmp);
        debugPort.printf("[keyboard] 0x%X\n", tmp);
    }
}

void loopSerial()
{
    // WARNING : No debug message should be used here

    bool endFlag = false;

    // minitel -> usb
    while (minitelPort.available() > 0)
    {
        byte inByte = minitelPort.read();
        if (inByte == 18 || (functionKey && inByte == 0x49))
        {
            // CTRL+R = RESET ou TS+CONNEXION
            endFlag = true;
        }
        functionKey = (inByte == 0x13);
        debugPort.write(inByte);
    }

    // usb -> minitel
    while (debugPort.available() > 0)
    {
        minitelPort.write(debugPort.read());
    }

    // end of serial loop
    if (endFlag)
    {
        debugPort.println();
        debugPort.println("*** Telnet Pro reset ***");
        if (!col80 && prestel) teletelMode();
        modeVideotex();
        minitel.newXY(1, 1);
        minitel.newScreen();
        minitel.echo(true);
        minitel.pageMode();
        reset();
    }
}

String inputString(const String& defaultValue, int& exitCode, const char padChar, int (*httpHandler)())
{
    String out = defaultValue == nullptr ? "" : defaultValue;
    minitel.print(out);
    minitel.cursor();
    unsigned long key = minitel.getKeyCode();
    while (!(
        key == 4929 || // Invio
        key == 13 || // CR
        key == 10 || // LF
        key == 27 || // ESC
        key == 3 || // CTRL+C
        key == 4934 // Sommaire
    ))
    {
        if (httpHandler != nullptr)
        {
            int handlerExitCode = httpHandler();
            if (handlerExitCode)
            {
                exitCode = 99;
                return "";
            }
        }

        if (key != 0)
        {
            debugPort.printf("Key = %lu\n", key);
            String str = minitel.getString(key);
            if (str != "")
            {
                out.concat(str);
                minitel.print(str);
            }
            else if (out.length() > 0 && (key == 8 || key == 4935))
            {
                // BACKSPACE
                unsigned int index = out.length() - 1;
                if (out.charAt(index) >> 7) // utf-8 multibyte pattern
                    while ((out.charAt(index) >> 6) != 0b11) index--; // utf-8 first byte pattern
                out.remove(index);
                minitel.noCursor();
                minitel.moveCursorLeft(1);
                minitel.printChar(padChar);
                minitel.moveCursorLeft(1);
                minitel.cursor();
            }
            else if (key == 18 || key == 4937)
            {
                // CTRL+R = RESET ou TS+CONNEXION
                modeVideotex();
                minitel.newXY(1, 1);
                minitel.newScreen();
                minitel.echo(true);
                minitel.pageMode();
                reset();
            }
            else if (key == 4933)
            {
                // ANNUL
                unsigned int length = numberOfChars(out);
                minitel.noCursor();
                for (int i = 0; i < length; ++i)
                {
                    minitel.moveCursorLeft(1);
                }
                for (int i = 0; i < length; ++i)
                {
                    minitel.printChar(padChar);
                }
                for (int i = 0; i < length; ++i)
                {
                    minitel.moveCursorLeft(1);
                }
                out = "";
                minitel.cursor();
            }
        }
        key = minitel.getKeyCode();
    }
    if (key == 3 || key == 27 || key == 4934)
        exitCode = 1;
    else
        exitCode = 0;
    minitel.noCursor();
    minitel.println();
    return out;
}

unsigned int numberOfChars(const String& str)
{
    // number of chars of a string including utf-8 multibyte characters
    unsigned int index = 0;
    unsigned int count = 0;
    while (index < str.length())
    {
        const byte car = str.charAt(index);
        if (car >> 5 == 0b110) index += 2; //utf-8 2 bytes pattern
        else if (car >> 4 == 0b1110) index += 3; // utf-8 3 bytes pattern
        else index++; //default (1 byte)
        count++;
    }
    return count;
}

void loadPrefs()
{
    prefs.begin("telnet-pro", true);
    debugPort.println("freeEntries = " + String(prefs.freeEntries()));
    ssid = prefs.getString("ssid", "");
    password = prefs.getString("password", "");
    url = prefs.getString("url", "");
    scroll = prefs.getBool("scroll", false);
    echo = prefs.getBool("echo", false);
    col80 = prefs.getBool("col80", false);
    prestel = prefs.getBool("prestel", false);
    altcharset = prefs.getBool("altcharset", false);
    privKey = prefs.getBool("privKey", false);
    connectionType = prefs.getUChar("connectionType", 0);
    ping_ms = prefs.getInt("ping_ms", 0);
    protocol = prefs.getString("protocol", "");
    sshUser = prefs.getString("sshUser", "");
    sshPass = prefs.getString("sshPass", "");
    sshPrivKey = prefs.getString("sshPrivKey", "");
    prefs.end();
}

void savePrefs()
{
    prefs.begin("telnet-pro", false);
    if (prefs.getString("ssid", "") != ssid) prefs.putString("ssid", ssid);
    if (prefs.getString("password", "") != password) prefs.putString("password", password);
    if (prefs.getString("url", "") != url) prefs.putString("url", url);
    if (prefs.getBool("scroll", false) != scroll) prefs.putBool("scroll", scroll);
    if (prefs.getBool("echo", false) != echo) prefs.putBool("echo", echo);
    if (prefs.getBool("col80", false) != col80) prefs.putBool("col80", col80);
    if (prefs.getBool("prestel", false) != prestel) prefs.putBool("prestel", prestel);
    if (prefs.getBool("altcharset", false) != altcharset) prefs.putBool("altcharset", altcharset);
    if (prefs.getBool("privKey", false) != privKey) prefs.putBool("privKey", privKey);
    if (prefs.getUChar("connectionType", 0) != connectionType) prefs.putUChar("connectionType", connectionType);
    if (prefs.getInt("ping_ms", 0) != ping_ms) prefs.putInt("ping_ms", ping_ms);
    if (prefs.getString("protocol", "") != protocol) prefs.putString("protocol", protocol);
    if (prefs.getString("sshUser", "") != sshUser) prefs.putString("sshUser", sshUser);
    if (prefs.getString("sshPass", "") != sshPass) prefs.putString("sshPass", sshPass);
    if (prefs.getString("sshPrivKey", "") != sshPrivKey) prefs.putString("sshPrivKey", sshPrivKey);
    prefs.end();
}

void showWifiStatus()
{
    String message;
    switch (wifiStatus)
    {
    case WIFI_WAITING: message = "CONNECT";
        break;
    case WIFI_CONNECTED: message = WiFi.localIP().toString();
        break;
    default: message = "NO SIGNAL";
        break;
    }
    minitel.newXY(21, 3);
    minitel.attributs(CARACTERE_ROUGE);
    if (wifiStatus == WIFI_WAITING) minitel.attributs(CLIGNOTEMENT);
    //else minitel.attributs(FIXE);
    for (int k = 0; k < 20 - message.length(); k++) minitel.printChar(' ');
    minitel.print(message);
}

void showPrefs()
{
    minitel.newScreen();
    minitel.textMode();
    minitel.noCursor();
    minitel.smallMode();
    minitel.attributs(GRANDEUR_NORMALE);
    minitel.attributs(CARACTERE_BLANC);
    minitel.attributs(FOND_NOIR);
    minitel.noCursor();
    minitel.newXY(1, 0);
    minitel.attributs(CARACTERE_ROUGE);
    minitel.print("?:HELP");
    minitel.cancel();
    minitel.moveCursorDown(1);
    minitel.newXY(9, 1);
    minitel.attributs(FIN_LIGNAGE);
    minitel.textMode();
    minitel.attributs(DOUBLE_HAUTEUR);
    minitel.attributs(CARACTERE_JAUNE);
    minitel.attributs(INVERSION_FOND);
    minitel.print("  Minitel Telnet Pro  ");
    minitel.newXY(34, 2);
    minitel.attributs(CARACTERE_ROUGE);
    minitel.print(String(speed));
    minitel.print("bps");
    showWifiStatus();
    minitel.newXY(1, 4);
    minitel.attributs(CARACTERE_BLANC);
    minitel.graphicMode();
    minitel.writeByte(0x6A);
    minitel.textMode();
    minitel.attributs(INVERSION_FOND);
    minitel.print("1");
    minitel.attributs(FOND_NORMAL);
    minitel.graphicMode();
    minitel.writeByte(0x35);
    minitel.textMode();
    minitel.print("SSID: ");
    minitel.attributs(CARACTERE_CYAN);
    printStringValue(ssid);
    clearLineFromCursor();
    minitel.println();
    minitel.attributs(CARACTERE_BLANC);
    minitel.graphicMode();
    minitel.writeByte(0x6A);
    minitel.textMode();
    minitel.attributs(INVERSION_FOND);
    minitel.print("2");
    minitel.attributs(FOND_NORMAL);
    minitel.graphicMode();
    minitel.writeByte(0x35);
    minitel.textMode();
    minitel.print("Pass: ");
    minitel.attributs(CARACTERE_CYAN);
    printPassword(password);
    clearLineFromCursor();
    minitel.println();
    minitel.newXY(1, 7);
    minitel.attributs(CARACTERE_BLANC);
    minitel.graphicMode();
    minitel.writeByte(0x6A);
    minitel.textMode();
    minitel.attributs(INVERSION_FOND);
    minitel.print("3");
    minitel.attributs(FOND_NORMAL);
    minitel.graphicMode();
    minitel.writeByte(0x35);
    minitel.textMode();
    minitel.print("URL: ");
    minitel.attributs(CARACTERE_CYAN);
    printStringValue(url);
    clearLineFromCursor();
    minitel.println();
    minitel.newXY(1, 9);
    minitel.attributs(CARACTERE_BLANC);
    minitel.graphicMode();
    minitel.writeByte(0x6A);
    minitel.textMode();
    minitel.attributs(INVERSION_FOND);
    minitel.print("4");
    minitel.attributs(FOND_NORMAL);
    minitel.graphicMode();
    minitel.writeByte(0x35);
    minitel.textMode();
    minitel.print("Scroll: ");
    writeBool(scroll);
    clearLineFromCursor();
    minitel.print("          ");
    minitel.attributs(CARACTERE_BLANC);
    minitel.graphicMode();
    minitel.writeByte(0x6A);
    minitel.textMode();
    minitel.attributs(INVERSION_FOND);
    minitel.print("C");
    minitel.attributs(FOND_NORMAL);
    minitel.graphicMode();
    minitel.writeByte(0x35);
    minitel.textMode();
    minitel.print("Prestel: ");
    writeBool(prestel);
    clearLineFromCursor();
    minitel.println();
    minitel.attributs(CARACTERE_BLANC);
    minitel.graphicMode();
    minitel.writeByte(0x6A);
    minitel.textMode();
    minitel.attributs(INVERSION_FOND);
    minitel.print("5");
    minitel.attributs(FOND_NORMAL);
    minitel.graphicMode();
    minitel.writeByte(0x35);
    minitel.textMode();
    minitel.print("Echo  : ");
    writeBool(echo);
    clearLineFromCursor();
    minitel.print("          ");
    minitel.attributs(CARACTERE_BLANC);
    minitel.graphicMode();
    minitel.writeByte(0x6A);
    minitel.textMode();
    minitel.attributs(INVERSION_FOND);
    minitel.print("A");
    minitel.attributs(FOND_NORMAL);
    minitel.graphicMode();
    minitel.writeByte(0x35);
    minitel.textMode();
    minitel.print("AltChar: ");
    writeBool(altcharset);
    clearLineFromCursor();
    minitel.println();
    minitel.attributs(CARACTERE_BLANC);
    minitel.graphicMode();
    minitel.writeByte(0x6A);
    minitel.textMode();
    minitel.attributs(INVERSION_FOND);
    minitel.print("6");
    minitel.attributs(FOND_NORMAL);
    minitel.graphicMode();
    minitel.writeByte(0x35);
    minitel.textMode();
    minitel.print("Col80 : ");
    writeBool(col80);
    clearLineFromCursor();
    minitel.println();
    minitel.newXY(1, 13);
    minitel.attributs(CARACTERE_BLANC);
    minitel.graphicMode();
    minitel.writeByte(0x6A);
    minitel.textMode();
    minitel.attributs(INVERSION_FOND);
    minitel.print("7");
    minitel.attributs(FOND_NORMAL);
    minitel.graphicMode();
    minitel.writeByte(0x35);
    minitel.textMode();
    minitel.print("Type    : ");
    writeConnectionType(connectionType); //clearLineFromCursor(); minitel.println();
    minitel.attributs(CARACTERE_BLANC);
    minitel.graphicMode();
    minitel.writeByte(0x6A);
    minitel.textMode();
    minitel.attributs(INVERSION_FOND);
    minitel.print("8");
    minitel.attributs(FOND_NORMAL);
    minitel.graphicMode();
    minitel.writeByte(0x35);
    minitel.textMode();
    minitel.print("PingMS  : ");
    minitel.attributs(CARACTERE_CYAN);
    minitel.print(String(ping_ms));
    clearLineFromCursor();
    minitel.println();
    minitel.attributs(CARACTERE_BLANC);
    minitel.graphicMode();
    minitel.writeByte(0x6A);
    minitel.textMode();
    minitel.attributs(INVERSION_FOND);
    minitel.print("9");
    minitel.attributs(FOND_NORMAL);
    minitel.graphicMode();
    minitel.writeByte(0x35);
    minitel.textMode();
    minitel.print("Subprot.: ");
    minitel.attributs(CARACTERE_CYAN);
    minitel.print(protocol);
    clearLineFromCursor();
    minitel.println();
    //minitel.newXY(1,16);
    minitel.attributs(CARACTERE_BLANC);
    minitel.graphicMode();
    minitel.writeByte(0x6A);
    minitel.textMode();
    minitel.attributs(INVERSION_FOND);
    minitel.print("U");
    minitel.attributs(FOND_NORMAL);
    minitel.graphicMode();
    minitel.writeByte(0x35);
    minitel.textMode();
    minitel.print("SSH User: ");
    minitel.attributs(CARACTERE_CYAN);
    minitel.print(sshUser);
    clearLineFromCursor();
    minitel.println();
    minitel.attributs(CARACTERE_BLANC);
    minitel.graphicMode();
    minitel.writeByte(0x6A);
    minitel.textMode();
    minitel.attributs(INVERSION_FOND);
    minitel.print("P");
    minitel.attributs(FOND_NORMAL);
    minitel.graphicMode();
    minitel.writeByte(0x35);
    minitel.textMode();
    minitel.print("SSH Pass: ");
    minitel.attributs(CARACTERE_CYAN);
    if (privKey)
    {
        minitel.print("-- privKey --");
    }
    else
    {
        if (sshPass != nullptr && sshPass != "") { printPassword(sshPass); }
    }
    clearLineFromCursor();
    minitel.println();

    minitel.newXY(1, 18);
    minitel.writeByte(0x5F);
    minitel.repeat(3);
    minitel.newXY(1, 19);
    minitel.print("{  }");
    minitel.newXY(1, 20);
    minitel.print("{  }");
    minitel.newXY(1, 21);
    minitel.writeByte(0x7E);
    minitel.repeat(3);
    minitel.newXY(2, 19);
    minitel.attributs(CARACTERE_BLANC);
    minitel.attributs(DOUBLE_GRANDEUR);
    minitel.print("S");
    minitel.newXY(6, 19);
    minitel.attributs(DOUBLE_HAUTEUR);
    minitel.print("Save Preset");

    int delta = 24;
    minitel.newXY(1 + delta, 18);
    minitel.writeByte(0x5F);
    minitel.repeat(3);
    minitel.newXY(1 + delta, 19);
    minitel.print("{  }");
    minitel.newXY(1 + delta, 20);
    minitel.print("{  }");
    minitel.newXY(1 + delta, 21);
    minitel.writeByte(0x7E);
    minitel.repeat(3);
    minitel.newXY(2 + delta, 19);
    minitel.attributs(CARACTERE_BLANC);
    minitel.attributs(DOUBLE_GRANDEUR);
    minitel.print("L");
    minitel.newXY(6 + delta, 19);
    minitel.attributs(DOUBLE_HAUTEUR);
    minitel.print("Load Preset");

    minitel.attributs(GRANDEUR_NORMALE);
    minitel.attributs(CARACTERE_JAUNE);
    minitel.newXY(1, 22);
    minitel.attributs(INVERSION_FOND);
    minitel.print(" SPACE ");
    minitel.attributs(FOND_NORMAL);
    minitel.print(" to connect   ");
    minitel.attributs(INVERSION_FOND);
    minitel.print(" CTRL+R ");
    minitel.attributs(FOND_NORMAL);
    minitel.print(" to restart");
    minitel.newXY(24, 23);
    minitel.print("or TS+CONNEXION");

    minitel.newXY(1, 24);
    minitel.attributs(CARACTERE_ROUGE);
    minitel.print("(C) 2023 Louis H. - Francesco Sblendorio");
    minitel.attributs(CARACTERE_BLANC);
}

void printPassword(const String& pwd)
{
    if (pwd == nullptr || pwd == "")
    {
        minitel.print("-undefined-");
    }
    else
    {
        minitel.graphicMode();
        minitel.attributs(DEBUT_LIGNAGE);
        for (int i = 0; i < numberOfChars(password); ++i) minitel.graphic(i <= 30 ? 0b001100 : 0b000000);
        minitel.attributs(FIN_LIGNAGE);
        minitel.textMode();
    }
}

void printStringValue(const String& s)
{
    if (s == nullptr || s == "")
    {
        minitel.print("-undefined-");
    }
    else
    {
        minitel.print(s);
    }
}

int setPrefs()
{
    unsigned long key = minitel.getKeyCode();
    unsigned long startMs = 0;
    wifiStatus = WIFI_BEGIN;
    bool valid = false;

    while (key != 32)
    {
        if (wifiStatus == WIFI_BEGIN)
        {
            wifiStatus = WIFI_WAITING;
            debugPort.println("Connecting to WiFi");
            showWifiStatus();
            WiFi.disconnect();
            WiFi.begin(ssid.c_str(), password.c_str());
            startMs = millis();
        }

        if (wifiStatus == WIFI_WAITING)
        {
            if (WiFiClass::status() == WL_CONNECTED)
            {
                wifiStatus = WIFI_CONNECTED;
                debugPort.println(" DONE");
                showWifiStatus();
            }
            else if (millis() - startMs > WIFI_TIMEOUT)
            {
                wifiStatus = WIFI_ABORTED;
                debugPort.println(" FAILED");
                showWifiStatus();
            }
            else
            {
                delay(200);
                debugPort.print('%');
            }
        }

        valid = false;
        if (key != 0)
        {
            valid = true;
            debugPort.printf("Key = %lu\n", key);
            if (key == 18 || key == 4937)
            {
                // CTRL+R = RESET ou TS+CONNEXION
                valid = false;
                modeVideotex();
                minitel.newXY(1, 1);
                minitel.newScreen();
                minitel.echo(true);
                minitel.pageMode();
                reset();
            }
            else if (key == '1')
            {
                if (setParameter(10, 4, ssid, false, false, nullptr) == 0) wifiStatus = WIFI_BEGIN;
            }
            else if (key == '2')
            {
                if (setParameter(10, 5, password, true, false, nullptr) == 0) wifiStatus = WIFI_BEGIN;
                if (password.length() <= 31)
                {
                    minitel.newXY(1, 6);
                    clearLineFromCursor();
                }
            }
            else if (key == '3')
            {
                setParameter(9, 7, url, false, false, nullptr);
                if (url.length() <= 40 - 9)
                {
                    minitel.newXY(1, 8);
                    clearLineFromCursor();
                }
            }
            else if (key == '4')
            {
                switchParameter(12, 9, scroll);
            }
            else if (key == '5')
            {
                switchParameter(12, 10, echo);
            }
            else if (key == '6')
            {
                switchParameter(12, 11, col80);
            }
            else if (key == 'a' || key == 'A')
            {
                switchParameter(37, 10, altcharset);
            }
            else if (key == 'c' || key == 'C')
            {
                switchParameter(37, 9, prestel);
            }
            else if (key == '7')
            {
                cycleConnectionType(14, 13);
            }
            else if (key == '8')
            {
                uint16_t temp = ping_ms;
                setIntParameter(14, 14, temp);
                ping_ms = temp;
            }
            else if (key == '9')
            {
                setParameter(14, 15, protocol, false, true, nullptr);
            }
            else if (key == 'u' || key == 'U')
            {
                setParameter(14, 16, sshUser, false, true, nullptr);
            }
            else if (key == 'p' || key == 'P')
            {
                if (serverStatus == HTTP_SERVER_CLOSED && WiFiClass::status() == WL_CONNECTED)
                {
                    debugPort.println("Server begin");
                    server.begin();
                    serverStatus = HTTP_SERVER_READY;
                }

                int inputExitCode = setParameter(14, 17, sshPass, true, true, manageHttpConnection);
                if (inputExitCode == 0)
                {
                    privKey = false;
                    sshPrivKey = "";
                }

                if (inputExitCode == 1 && privKey)
                {
                    minitel.newXY(14, 17);
                    minitel.attributs(CARACTERE_CYAN);
                    minitel.print("-- privKey --");
                    clearLineFromCursor();
                    minitel.println();
                }

                debugPort.println("Server end");
                serverStatus = HTTP_SERVER_CLOSED;
                server.end();
            }
            else if (key == 's' || key == 'S')
            {
                savePresets();
            }
            else if (key == 'l' || key == 'L')
            {
                loadPresets();
            }
            else if (key == '?')
            {
                showHelp();
            }
            else
            {
                valid = false;
            }
        }
        if (valid)
        {
            savePrefs();
        }
        key = minitel.getKeyCode();
    }
    //server.end();
    debugPort.println("Server end");
    serverStatus = HTTP_SERVER_CLOSED;
    minitel.newXY(1, 0);
    minitel.cancel();
    minitel.newScreen();
    return 0;
}

void savePresets()
{
    uint32_t key;
    displayPresets("Save to slot");
    do
    {
        minitel.newXY(1, 24);
        minitel.attributs(CARACTERE_VERT);
        minitel.print("  Choose slot, ESC or SUMMARY to go back");
        minitel.smallMode();
        while ((key = minitel.getKeyCode()) == 0)
        {
        }
        if (key == 27 || key == 4933 || key == 4934)
        {
            break;
        }
        if (key == 18 || key == 4937)
        {
            // CTRL+R = RESET ou TS+CONNEXION
            reset();
        }
        else if ((key | 32) >= 'a' && (key | 32) <= 'a' + 20 - 1)
        {
            const int slot = static_cast<int>(key | 32) - 'a';
            debugPort.printf("slot = %d\n", slot);
            minitel.newXY(1, 24);
            minitel.attributs(CARACTERE_VERT);
            minitel.print("      Name your slot, ESC to cancel");
            clearLineFromCursor();
            String presetName(presets[slot].presetName);
            const int exitCode = setParameter(3, 4 + slot, presetName, false, true, nullptr);
            if (exitCode)
            {
                continue;
            }
            if (presetName == "")
            {
                minitel.attributs(CARACTERE_CYAN);
                minitel.newXY(3, 4 + slot);
                minitel.print(presets[slot].presetName);
                continue;
            }
            // save preset
            presets[slot].presetName = presetName;
            presets[slot].url = url;
            presets[slot].scroll = scroll;
            presets[slot].echo = echo;
            presets[slot].col80 = col80;
            presets[slot].prestel = prestel;
            presets[slot].altcharset = altcharset;
            presets[slot].privKey = privKey;
            presets[slot].connectionType = connectionType;
            presets[slot].ping_ms = ping_ms;
            presets[slot].protocol = protocol;
            presets[slot].sshUser = sshUser;
            presets[slot].sshPass = sshPass;
            presets[slot].sshPrivKey = sshPrivKey;
            writePresets();
        }
    }
    while (true);
    showPrefs();
}

void loadPresets()
{
    uint32_t key;
    displayPresets("Load from slot");
    do
    {
        minitel.newXY(1, 24);
        minitel.attributs(CARACTERE_VERT);
        minitel.print("  Choose slot, ESC or SUMMARY to go back");
        minitel.smallMode();
        while ((key = minitel.getKeyCode()) == 0)
        {
        }
        if (key == 27 || key == 4933 || key == 4934)
        {
            break;
        }
        if (key == 18 || key == 4937)
        {
            // CTRL+R = RESET ou TS+CONNEXION
            reset();
        }
        else if ((key | 32) >= 'a' && (key | 32) <= 'a' + 20 - 1)
        {
            const int slot = static_cast<int>(key | 32) - 'a';
            debugPort.printf("slot = %d\n", slot);
            if (presets[slot].presetName == "")
            {
                continue;
            }

            minitel.attributs(CARACTERE_BLANC);
            minitel.attributs(INVERSION_FOND);
            minitel.newXY(3, 4 + slot);
            minitel.print(presets[slot].presetName);
            delay(500);

            url = presets[slot].url;
            scroll = presets[slot].scroll;
            echo = presets[slot].echo;
            col80 = presets[slot].col80;
            prestel = presets[slot].prestel;
            altcharset = presets[slot].altcharset;
            privKey = presets[slot].privKey;
            connectionType = presets[slot].connectionType;
            ping_ms = presets[slot].ping_ms;
            protocol = presets[slot].protocol;
            sshUser = presets[slot].sshUser;
            sshPass = presets[slot].sshPass;
            sshPrivKey = presets[slot].sshPrivKey;

            minitel.attributs(CARACTERE_CYAN);
            minitel.attributs(FOND_NORMAL);
            minitel.newXY(3, 4 + slot);
            minitel.print(presets[slot].presetName);

            break;
        }
    }
    while (true);
    showPrefs();
}

void displayPresets(const String& title)
{
    String alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    minitel.newScreen();
    minitel.newXY(1, 1);
    minitel.attributs(DOUBLE_HAUTEUR);
    minitel.attributs(CARACTERE_CYAN);
    minitel.print(title);
    minitel.newXY(1, 4);
    for (int i = 0; i < 20; ++i)
    {
        minitel.attributs(CARACTERE_BLANC);
        minitel.printChar(alphabet[i]);
        minitel.print(":");
        minitel.attributs(CARACTERE_CYAN);
        minitel.println(presets[i].presetName);
    }
}

void cycleConnectionType(const int x, const int y)
{
    connectionType = (connectionType + 1) % 4;
    minitel.newXY(x, y);
    writeConnectionType(connectionType);
}

void switchParameter(const int x, const int y, bool& destination)
{
    destination = !destination;
    minitel.newXY(x, y);
    writeBool(destination);
}

int setParameter(const int x, const int y, String& destination, const bool mask, const bool allowBlank,
                 int (*httpHandler)())
{
    minitel.newXY(x, y);
    minitel.attributs(CARACTERE_BLANC);
    minitel.print(destination);
    int len = 41 - x - static_cast<int>(numberOfChars(destination));
    debugPort.printf("************ %d ***********\n", len);
    if (len < 0) len = 0;
    for (int i = 0; i < len; ++i) minitel.print(".");
    minitel.newXY(x, y);
    int exitCode = 0;
    String temp = inputString(destination, exitCode, '.', httpHandler);
    if (!exitCode)
    {
        if (allowBlank || temp.length() > 0)
        {
            destination = String(temp);
        }
    }
    if (exitCode == 99) return exitCode;

    minitel.newXY(x, y);
    minitel.attributs(CARACTERE_CYAN);
    if (destination == "")
    {
        if (!allowBlank) minitel.print("-undefined-");
    }
    else if (httpHandler && exitCode == 1 && privKey)
    {
        // do nothing
    }
    else
    {
        if (mask)
        {
            minitel.graphicMode();
            minitel.attributs(DEBUT_LIGNAGE);
            for (int i = 0; i < numberOfChars(destination); ++i) minitel.graphic(i <= 30 ? 0b001100 : 0b000000);
            minitel.attributs(FIN_LIGNAGE);
            minitel.textMode();
        }
        else
            printStringValue(destination);
    }
    clearLineFromCursor();
    return exitCode;
}

void setIntParameter(const int x, const int y, uint16_t& destination)
{
    auto strParam = String(destination);
    if (strParam == "0") strParam = "";
    minitel.newXY(x, y);
    minitel.attributs(CARACTERE_BLANC);
    minitel.print(strParam);
    for (int i = 0; i < 41 - x - String(destination).length(); ++i) minitel.print(".");
    minitel.newXY(x, y);
    int exitCode = 0;
    const String temp = inputString(strParam, exitCode, '.', nullptr);
    if (!exitCode && temp.length() > 0)
    {
        destination = temp.toInt();
    }
    minitel.newXY(x, y);
    minitel.attributs(CARACTERE_CYAN);
    minitel.print(String(destination));
    clearLineFromCursor();
}

void writeBool(const bool value)
{
    if (value)
    {
        minitel.attributs(CARACTERE_VERT);
        minitel.print("Yes");
    }
    else
    {
        minitel.attributs(CARACTERE_ROUGE);
        minitel.print("No ");
    }
    minitel.attributs(CARACTERE_BLANC);
}

void writeConnectionType(const byte connType)
{
    if (connType == 0)
    {
        minitel.attributs(CARACTERE_BLANC);
        minitel.attributs(INVERSION_FOND);
    }
    else
    {
        minitel.attributs(CARACTERE_ROUGE);
        minitel.attributs(FOND_NORMAL);
    }
    minitel.print("Telnet");
    minitel.attributs(CARACTERE_ROUGE);
    minitel.attributs(FOND_NORMAL);
    minitel.print("/");

    if (connType == 1)
    {
        minitel.attributs(CARACTERE_BLANC);
        minitel.attributs(INVERSION_FOND);
    }
    else
    {
        minitel.attributs(CARACTERE_ROUGE);
        minitel.attributs(FOND_NORMAL);
    }
    minitel.print("Websocket");

    minitel.attributs(CARACTERE_ROUGE);
    minitel.attributs(FOND_NORMAL);
    minitel.print("/");

    if (connType == 2)
    {
        minitel.attributs(CARACTERE_BLANC);
        minitel.attributs(INVERSION_FOND);
    }
    else
    {
        minitel.attributs(CARACTERE_ROUGE);
        minitel.attributs(FOND_NORMAL);
    }
    minitel.print("SSH");

    minitel.attributs(CARACTERE_ROUGE);
    minitel.attributs(FOND_NORMAL);
    minitel.print("/");

    if (connType == 3)
    {
        minitel.attributs(CARACTERE_BLANC);
        minitel.attributs(INVERSION_FOND);
    }
    else
    {
        minitel.attributs(CARACTERE_ROUGE);
        minitel.attributs(FOND_NORMAL);
    }
    minitel.print("Serial");

    minitel.attributs(CARACTERE_BLANC);
    minitel.attributs(FOND_NORMAL);
}

void separateUrl(String urlToSeparate)
{
    urlToSeparate.trim();
    auto temp = String(urlToSeparate);
    temp.toLowerCase();

    ssl = false;

    if (temp.startsWith("wss://"))
    {
        ssl = true;
        urlToSeparate.remove(0, 6);
    }
    else if (temp.startsWith("ws://"))
    {
        ssl = false;
        urlToSeparate.remove(0, 5);
    }
    else if (temp.startsWith("wss:"))
    {
        ssl = true;
        urlToSeparate.remove(0, 4);
    }
    else if (temp.startsWith("ws:"))
    {
        ssl = false;
        urlToSeparate.remove(0, 3);
    }
    else if (temp.startsWith("ssh://"))
    {
        urlToSeparate.remove(0, 6);
    }
    else if (temp.startsWith("ssh:"))
    {
        urlToSeparate.remove(0, 4);
    }

    int colon = urlToSeparate.indexOf(':');
    int slash = urlToSeparate.indexOf('/');

    if (slash == -1 && colon == -1)
    {
        host = urlToSeparate;
        port = 0;
        path = "/";
    }
    else if (slash == -1 && colon != -1)
    {
        host = urlToSeparate.substring(0, colon);
        port = urlToSeparate.substring(colon + 1).toInt();
        path = "/";
    }
    else if (slash != -1 && colon == -1)
    {
        host = urlToSeparate.substring(0, slash);
        port = 0;
        path = urlToSeparate.substring(slash);
    }
    else if (slash != -1 && colon != -1)
    {
        host = urlToSeparate.substring(0, colon);
        port = urlToSeparate.substring(colon + 1, slash).toInt();
        path = urlToSeparate.substring(slash);
    }

    if (port == 0)
    {
        if (connectionType == 0)
        {
            port = 23;
        }
        else if (connectionType == 1)
        {
            port = ssl ? 443 : 80;
        }
        else if (connectionType == 2)
        {
            port = 22;
        }
    }
}

void loopSsh()
{
    // do nothing while sshTask runs
    if (eTaskGetState(sshTaskHandle) != eDeleted)
    {
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }
    else
    {
        // reset otherwise
        if (!col80 && prestel) teletelMode();
        reset();
    }
}

void sshTask(void* pvParameters)
{
    debugPort.printf("\n> SSH task running\n");

    // Open ssh session
    debugPort.printf("  Connecting to %s as %s\n", host.c_str(), sshUser.c_str());
    bool isOpen = sshClient.begin(host.c_str(), port, sshUser.c_str(), sshPass.c_str(), privKey, sshPrivKey.c_str());
    if (!isOpen) debugPort.printf("  > SSH authentication failed\n");

    // Loop task
    while (true)
    {
        bool cancel = false;
        // Check ssh channel
        if (!sshClient.available())
        {
            debugPort.printf("ssh channel lost\n");
            break;
        }

        // host -> minitel
        int nbytes = sshClient.receive();
        if (nbytes < 0)
        {
            debugPort.printf("  > SSH Error while receiving\n");
            break;
        }
        if (nbytes > 0)
        {
            debugPort.printf("[SSH] got %u bytes\n", nbytes);
            int index = 0;
            while (index < nbytes)
            {
                char b = sshClient.readIndex(index++);
                if (b <= DEL) minitel.writeByte(b); // print only code < 128
                else
                {
                    // replace char with one "?"
                    minitel.writeByte('?');
                    // increment index considering utf-8 encoding
                    if (b < 0b11100000) index += 1;
                    else if (b < 0b11110000) index += 2;
                    else index += 3;
                }
            }
        }

        // minitel -> host
        uint32_t key = minitel.getKeyCode(false);
        if (key == 0)
        {
            vTaskDelay(50 / portTICK_PERIOD_MS);
            continue;
        }
        else if (key == 18 || key == 4937)
        {
            // CTRL+R = RESET ou TS+CONNEXION
            if (!col80 && prestel) teletelMode();
            break;
        }
        debugPort.printf("[KB] got code: 0x%X\n", key);
        switch (key)
        {
        // redirect minitel special keys
        case SOMMAIRE:
        case GUIDE:
            key = 0x07;
            break; //BEL : ring
        case ANNULATION: key = 0x0515;
            break; //ctrl+E ctrl+U : end of line + remove left
        case CORRECTION: key = 0x7F;
            break; //DEL : delete
        case RETOUR: key = 0x01;
            break; //ctrl+A : beginning of line
        case SUITE: key = 0x05;
            break; //ctrl+E : end of line
        case REPETITION: key = 0x0C;
            break; //ctrl+L : clear-screen (current command repeated)
        case ENVOI: key = 0x0D;
            break; //CR : validate command
        // intercept ctrl+c
        case 0x03: cancel = true;
            break;
        default:
            break;
        }
        // prepare data to send over ssh
        uint8_t payload[4];
        size_t len = 0;
        for (len = 0; key != 0 && len < 4; len++)
        {
            payload[3 - len] = static_cast<uint8_t>(key);
            key = key >> 8;
        }
        if (sshClient.send(payload + 4 - len, len) < 0)
        {
            debugPort.printf("  > SSH Error while sending\n");
            break;
        }
        // Intercept CTRL+C:
        // displaying data received before host get the command can take minutes
        // we ignore received data to avoid this
        if (cancel)
        {
            debugPort.printf(" > Intercepted ctrl+C\n");
            int nbyte = sshClient.flushReceiving();
            if (col80)
            {
                minitel.writeByte(0x1b);
                minitel.println("[0m"); // Reset ANSI/VT100 attributes
                minitel.writeByte(altcharset ? 0x0e : 0x0f); // US ASCII Charset in 80 columns
            }
            minitel.println();
            minitel.println("\r\r * ctrl+C * ");
            minitel.print("Warning: ");
            minitel.print(String(nbyte));
            minitel.println(" received bytes ignored ");
            minitel.println("as it may takes minutes to display on minitel.");
            // send CR to get new input line
            uint8_t cr = 0x0D;
            int res = sshClient.send(&cr, 1);
            debugPort.printf("CR sent: %d", res);
        }
    }
    // Closing session
    debugPort.printf(" >  Session closed\n");
    sshClient.end();

    // Reinit minitel and Self delete ssh task
    debugPort.printf("\n> SSH task end\n");

    modeVideotex();
    minitel.newXY(1, 1);
    minitel.newScreen();
    minitel.echo(true);
    minitel.pageMode();
    vTaskDelete(nullptr);
}


void loopWebsocket()
{
    // Websocket -> Minitel
    webSocket.loop();

    // Minitel -> Websocket
    uint32_t key = minitel.getKeyCode(false);
    if (key != 0)
    {
        if (key == 18 || key == 4937)
        {
            // CTRL+R = RESET ou TS+CONNEXION
            webSocket.disconnect();
            if (!col80 && prestel) teletelMode();
            modeVideotex();
            minitel.newXY(1, 1);
            minitel.newScreen();
            minitel.echo(true);
            minitel.pageMode();
            reset();
        }
        debugPort.printf("[KB] got code: 0x%X\n", key);
        // prepare data to send over websocket
        uint8_t payload[4];
        size_t len = 0;
        for (len = 0; key != 0 && len < 4; len++)
        {
            payload[3 - len] = static_cast<uint8_t>(key);
            key = key >> 8;
        }
        webSocket.sendTXT(payload + 4 - len, len);
    }
}

void webSocketEvent(const WStype_t type, uint8_t* payload, const size_t len)
{
    switch (type)
    {
    case WStype_DISCONNECTED:
        debugPort.printf("[WS] Disconnected!\n");
        /*
              minitel.println();
              minitel.println("DISCONNECTING...");
              delay(3000);
              webSocket.disconnect();
              // WiFi.disconnect();
              if (!col80 && prestel) teletelMode();
              reset();
        */
        break;

    case WStype_CONNECTED:
        debugPort.printf("[WS] Connected to url: %p\n", payload);
        break;

    case WStype_TEXT:
        debugPort.printf("[WS] got %u chars\n", len);
        if (len > 0)
        {
            debugPort.printf("  >  %p\n", payload);
            for (size_t i = 0; i < len; i++)
            {
                minitel.writeByte(payload[i]);
            }
        }
        break;

    case WStype_BIN:
        debugPort.printf("[WS] got %u binaries\n", len);
        if (len > 0)
        {
            debugPort.printf("  >  %p\n", payload);
            for (size_t i = 0; i < len; i++)
            {
                minitel.writeByte(payload[i]);
            }
        }
        break;

    case WStype_ERROR:
        debugPort.printf("[WS] WStype_ERROR\n");
        break;

    case WStype_FRAGMENT_TEXT_START:
        debugPort.printf("[WS] WStype_FRAGMENT_TEXT_START\n");
        break;

    case WStype_FRAGMENT_BIN_START:
        debugPort.printf("[WS] WStype_FRAGMENT_BIN_START\n");
        break;

    case WStype_FRAGMENT:
        debugPort.printf("[WS] WStype_FRAGMENT\n");
        break;

    case WStype_FRAGMENT_FIN:
        debugPort.printf("[WS] WStype_FRAGMENT_FIN\n");
        break;
    default:
        break;
    }

}

void writePresets()
{
    initFS();
    File file = SPIFFS.open("/telnetpro-presets.cnf", FILE_WRITE);

    for (const auto& preset : presets)
    {
        auto doc = JsonDocument();
        doc["presetName"] = preset.presetName;
        doc["url"] = preset.url;
        doc["scroll"] = preset.scroll;
        doc["echo"] = preset.echo;
        doc["col80"] = preset.col80;
        doc["prestel"] = preset.prestel;
        doc["altcharset"] = preset.altcharset;
        doc["privKey"] = preset.privKey;
        doc["connectionType"] = preset.connectionType;
        doc["ping_ms"] = preset.ping_ms;
        doc["protocol"] = preset.protocol;
        doc["sshUser"] = preset.sshUser;
        doc["sshPass"] = preset.sshPass;
        doc["sshPrivKey"] = preset.sshPrivKey;

        if (serializeJson(doc, file) == 0)
        {
            debugPort.println(F("Failed to write to file"));
        }
    }
    file.close();
    SPIFFS.end();
}

void readPresets()
{
    int countNonEmptySlots = 0;
    initFS();
    File file = SPIFFS.open("/telnetpro-presets.cnf", FILE_READ);
    auto doc = JsonDocument();
    for (auto& preset : presets)
    {
        DeserializationError error = deserializeJson(doc, file);
        if (error)
        {
            preset.presetName = "";
            preset.url = "";
            preset.scroll = false;
            preset.echo = false;
            preset.col80 = false;
            preset.prestel = false;
            preset.altcharset = false;
            preset.privKey = false;
            preset.connectionType = 0;
            preset.ping_ms = 0;
            preset.protocol = "";
            preset.sshUser = "";
            preset.sshPass = "";
            preset.sshPrivKey = "";
        }
        else
        {
            String _presetName = doc["presetName"];
            preset.presetName = _presetName == "null" ? "" : _presetName;
            String _url = doc["url"];
            preset.url = _url == "null" ? "" : _url;
            bool _scroll = doc["scroll"];
            preset.scroll = _scroll;
            bool _echo = doc["echo"];
            preset.echo = _echo;
            bool _col80 = doc["col80"];
            preset.col80 = _col80;
            bool _prestel = doc["prestel"];
            preset.prestel = _prestel;
            bool _altcharset = doc["altcharset"];
            preset.altcharset = _altcharset;
            bool _privKey = doc["privKey"];
            preset.privKey = _privKey;
            byte _connectionType = doc["connectionType"];
            preset.connectionType = _connectionType;
            int _ping_ms = doc["ping_ms"];
            preset.ping_ms = _ping_ms;
            String _protocol = doc["protocol"];
            preset.protocol = _protocol == "null" ? "" : _protocol;
            String _sshUser = doc["sshUser"];
            preset.sshUser = _sshUser == "null" ? "" : _sshUser;
            String _sshPass = doc["sshPass"];
            preset.sshPass = _sshPass == "null" ? "" : _sshPass;
            String _sshPrivKey = doc["sshPrivKey"];
            preset.sshPrivKey = _sshPrivKey == "null" ? "" : _sshPrivKey;

            ++countNonEmptySlots;
        }
    }

    file.close();
    SPIFFS.end();

    if (countNonEmptySlots == 0)
    {
        // DEFAULT PRESETS IF THERE IS NO PRESET
        presets[0].presetName = "Retrocampus BBS";
        presets[0].url = "bbs.retrocampus.com:1651";
        presets[0].scroll = true;
        presets[0].echo = false;
        presets[0].col80 = false;
        presets[0].prestel = false;
        presets[0].altcharset = false;
        presets[0].privKey = false;
        presets[0].connectionType = 0; // Telnet
        presets[0].ping_ms = 0;
        presets[0].protocol = "";
        presets[0].sshUser = "";
        presets[0].sshPass = "";
        presets[0].sshPrivKey = "";

        presets[1].presetName = "3614 HACKER";
        presets[1].url = "ws:mntl.joher.com:2018/?echo";
        presets[1].scroll = false;
        presets[1].echo = false;
        presets[1].col80 = false;
        presets[1].prestel = false;
        presets[1].altcharset = false;
        presets[1].privKey = false;
        presets[1].connectionType = 1; // Websocket
        presets[1].ping_ms = 0;
        presets[1].protocol = "";
        presets[1].sshUser = "";
        presets[1].sshPass = "";
        presets[1].sshPrivKey = "";

        presets[2].presetName = "3614 TEASER";
        presets[2].url = "ws:minitel.3614teaser.fr:8080/ws";
        presets[2].scroll = false;
        presets[2].echo = false;
        presets[2].col80 = false;
        presets[2].prestel = false;
        presets[2].altcharset = false;
        presets[2].privKey = false;
        presets[2].connectionType = 1; // Websocket
        presets[2].ping_ms = 10000;
        presets[2].protocol = "tty";
        presets[2].sshUser = "";
        presets[2].sshPass = "";
        presets[2].sshPrivKey = "";

        presets[3].presetName = "3615 SM";
        presets[3].url = "wss:wss.3615.live:9991/?echo";
        presets[3].scroll = false;
        presets[3].echo = false;
        presets[3].col80 = false;
        presets[3].prestel = false;
        presets[3].altcharset = false;
        presets[3].privKey = false;
        presets[3].connectionType = 1; // Websocket
        presets[3].ping_ms = 0;
        presets[3].protocol = "";
        presets[3].sshUser = "";
        presets[3].sshPass = "";
        presets[3].sshPrivKey = "";

        presets[4].presetName = "3611.re";
        presets[4].url = "ws:3611.re/ws";
        presets[4].scroll = false;
        presets[4].echo = false;
        presets[4].col80 = false;
        presets[4].prestel = false;
        presets[4].altcharset = false;
        presets[4].privKey = false;
        presets[4].connectionType = 1; // Websocket
        presets[4].ping_ms = 0;
        presets[4].protocol = "";
        presets[4].sshUser = "";
        presets[4].sshPass = "";
        presets[4].sshPrivKey = "";

        presets[5].presetName = "3615co.de";
        presets[5].url = "ws:3615co.de/ws";
        presets[5].scroll = false;
        presets[5].echo = false;
        presets[5].col80 = false;
        presets[5].prestel = false;
        presets[5].altcharset = false;
        presets[5].privKey = false;
        presets[5].connectionType = 1; // Websocket
        presets[5].ping_ms = 0;
        presets[5].protocol = "";
        presets[5].sshUser = "";
        presets[5].sshPass = "";
        presets[5].sshPrivKey = "";

        presets[6].presetName = "miniPAVI";
        presets[6].url = "ws://go.minipavi.fr:8182";
        presets[6].scroll = false;
        presets[6].echo = false;
        presets[6].col80 = false;
        presets[6].prestel = false;
        presets[6].altcharset = false;
        presets[6].privKey = false;
        presets[6].connectionType = 1; // ws
        presets[6].ping_ms = 0;
        presets[6].protocol = "";
        presets[6].sshUser = "";
        presets[6].sshPass = "";
        presets[6].sshPrivKey = "";

        presets[7].presetName = "TELSTAR by GlassTTY";
        presets[7].url = "glasstty.com:6502";
        presets[7].scroll = false;
        presets[7].echo = false;
        presets[7].col80 = false;
        presets[7].prestel = true;
        presets[7].altcharset = true;
        presets[7].privKey = true;
        presets[7].connectionType = 0; // Telnet
        presets[7].ping_ms = 0;
        presets[7].protocol = "";
        presets[7].sshUser = "";
        presets[7].sshPass = "";
        presets[7].sshPrivKey = "";

        presets[8].presetName = "ssh example";
        presets[8].url = "[host name or ip]";
        presets[8].scroll = true;
        presets[8].echo = false;
        presets[8].col80 = true;
        presets[8].prestel = false;
        presets[8].altcharset = false;
        presets[8].privKey = false;
        presets[8].connectionType = 2; // SSH
        presets[8].ping_ms = 0;
        presets[8].protocol = "";
        presets[8].sshUser = "pi";
        presets[8].sshPass = "raspberry";
        presets[8].sshPrivKey = "";

        writePresets();
    }
}

void reset()
{
    ESP.restart();
}

void teletelMode()
{
    if (!advanced) return;
    minitel.writeByte(27);
    minitel.writeByte(37);
    minitel.writeByte(68);
    minitel.writeByte(97);
    // minitel.writeByte(64);
}

void prestelMode()
{
    if (!advanced) return;
    minitel.writeByte(27);
    minitel.writeByte(37);
    minitel.writeByte(68);
    minitel.writeByte(98);
    // minitel.writeByte(64);
}

void modeVideotex()
{
    if (advanced) minitel.modeVideotex();
}

void modeMixte()
{
    if (advanced) minitel.modeMixte();
}

void extendedKeyboard()
{
    if (advanced) minitel.extendedKeyboard();
}

void clearLineFromCursor()
{
    if (advanced) minitel.clearLineFromCursor();
}

void showHelp()
{
    minitel.textMode();
    minitel.noCursor();
    minitel.smallMode();
    minitel.newXY(1, 0);
    minitel.attributs(CARACTERE_ROUGE);
    minitel.println("HELP PAGE");
    minitel.cancel();
    minitel.moveCursorDown(1);
    minitel.newScreen();
    minitel.newXY(1, 2);
    minitel.attributs(CARACTERE_ROUGE);
    minitel.attributs(DEBUT_LIGNAGE);
    minitel.print(" Wifi settings (not used for Serial)    ");
    minitel.attributs(FIN_LIGNAGE);
    minitel.attributs(CARACTERE_BLANC);
    minitel.graphicMode();
    minitel.writeByte(0x6A);
    minitel.textMode();
    minitel.attributs(INVERSION_FOND);
    minitel.print("1");
    minitel.attributs(FOND_NORMAL);
    minitel.graphicMode();
    minitel.writeByte(0x35);
    minitel.textMode();
    minitel.print("SSID: ");
    minitel.attributs(CARACTERE_CYAN);
    minitel.println("name of your wifi network");
    minitel.attributs(CARACTERE_BLANC);
    minitel.graphicMode();
    minitel.writeByte(0x6A);
    minitel.textMode();
    minitel.attributs(INVERSION_FOND);
    minitel.print("2");
    minitel.attributs(FOND_NORMAL);
    minitel.graphicMode();
    minitel.writeByte(0x35);
    minitel.textMode();
    minitel.print("Pass: ");
    minitel.attributs(CARACTERE_CYAN);
    minitel.println("your password");
    minitel.newXY(1, 6);
    minitel.attributs(CARACTERE_ROUGE);
    minitel.attributs(DEBUT_LIGNAGE);
    minitel.print(" Server settings (not used for Serial)  ");
    minitel.attributs(FIN_LIGNAGE);
    minitel.attributs(CARACTERE_BLANC);
    minitel.graphicMode();
    minitel.writeByte(0x6A);
    minitel.textMode();
    minitel.attributs(INVERSION_FOND);
    minitel.print("3");
    minitel.attributs(FOND_NORMAL);
    minitel.graphicMode();
    minitel.writeByte(0x35);
    minitel.textMode();
    minitel.print("URL: ");
    minitel.attributs(CARACTERE_CYAN);
    minitel.println("url or ip address (:port number)");
    minitel.newXY(1, 9);
    minitel.attributs(CARACTERE_ROUGE);
    minitel.attributs(DEBUT_LIGNAGE);
    minitel.print(" Minitel settings ");
    minitel.repeat(22);
    minitel.attributs(FIN_LIGNAGE);
    minitel.attributs(CARACTERE_BLANC);
    minitel.graphicMode();
    minitel.writeByte(0x6A);
    minitel.textMode();
    minitel.attributs(INVERSION_FOND);
    minitel.print("4");
    minitel.attributs(FOND_NORMAL);
    minitel.graphicMode();
    minitel.writeByte(0x35);
    minitel.textMode();
    minitel.print("Scroll: ");
    minitel.attributs(CARACTERE_CYAN);
    minitel.println("scroll mode or page mode");
    minitel.attributs(CARACTERE_BLANC);
    minitel.graphicMode();
    minitel.writeByte(0x6A);
    minitel.textMode();
    minitel.attributs(INVERSION_FOND);
    minitel.print("5");
    minitel.attributs(FOND_NORMAL);
    minitel.graphicMode();
    minitel.writeByte(0x35);
    minitel.textMode();
    minitel.print("Echo  : ");
    minitel.attributs(CARACTERE_CYAN);
    minitel.println("enable or disable local echo");
    minitel.attributs(CARACTERE_BLANC);
    minitel.graphicMode();
    minitel.writeByte(0x6A);
    minitel.textMode();
    minitel.attributs(INVERSION_FOND);
    minitel.print("6");
    minitel.attributs(FOND_NORMAL);
    minitel.graphicMode();
    minitel.writeByte(0x35);
    minitel.textMode();
    minitel.print("Col80 : ");
    minitel.attributs(CARACTERE_CYAN);
    minitel.println("80 columns - for 1b or later");
    minitel.attributs(CARACTERE_BLANC);
    minitel.graphicMode();
    minitel.writeByte(0x6A);
    minitel.textMode();
    minitel.attributs(INVERSION_FOND);
    minitel.print("C");
    minitel.attributs(FOND_NORMAL);
    minitel.graphicMode();
    minitel.writeByte(0x35);
    minitel.textMode();
    minitel.print("Prestel: ");
    minitel.attributs(CARACTERE_CYAN);
    minitel.println("only for videotel terminals");
    minitel.attributs(CARACTERE_BLANC);
    minitel.graphicMode();
    minitel.writeByte(0x6A);
    minitel.textMode();
    minitel.attributs(INVERSION_FOND);
    minitel.print("A");
    minitel.attributs(FOND_NORMAL);
    minitel.graphicMode();
    minitel.writeByte(0x35);
    minitel.textMode();
    minitel.print("AltChar: ");
    minitel.attributs(CARACTERE_CYAN);
    minitel.println("only for videotel terminals");
    minitel.newXY(1, 16);
    minitel.attributs(CARACTERE_ROUGE);
    minitel.attributs(DEBUT_LIGNAGE);
    minitel.print(" Connection settings ");
    minitel.repeat(19);
    minitel.attributs(FIN_LIGNAGE);
    minitel.attributs(CARACTERE_BLANC);
    minitel.graphicMode();
    minitel.writeByte(0x6A);
    minitel.textMode();
    minitel.attributs(INVERSION_FOND);
    minitel.print("7");
    minitel.attributs(FOND_NORMAL);
    minitel.graphicMode();
    minitel.writeByte(0x35);
    minitel.textMode();
    minitel.print("Type    : ");
    minitel.attributs(CARACTERE_CYAN);
    minitel.println("connection type");
    minitel.attributs(CARACTERE_BLANC);
    minitel.graphicMode();
    minitel.writeByte(0x6A);
    minitel.textMode();
    minitel.attributs(INVERSION_FOND);
    minitel.print("8");
    minitel.attributs(FOND_NORMAL);
    minitel.graphicMode();
    minitel.writeByte(0x35);
    minitel.textMode();
    minitel.print("PingMS  : ");
    minitel.attributs(CARACTERE_CYAN);
    minitel.print("heartbeat in ms (websocket)");
    minitel.attributs(CARACTERE_BLANC);
    minitel.graphicMode();
    minitel.writeByte(0x6A);
    minitel.textMode();
    minitel.attributs(INVERSION_FOND);
    minitel.print("9");
    minitel.attributs(FOND_NORMAL);
    minitel.graphicMode();
    minitel.writeByte(0x35);
    minitel.textMode();
    minitel.print("Subprot.: ");
    minitel.attributs(CARACTERE_CYAN);
    minitel.println("subprotocol (websocket)");
    minitel.attributs(CARACTERE_BLANC);
    minitel.graphicMode();
    minitel.writeByte(0x6A);
    minitel.textMode();
    minitel.attributs(INVERSION_FOND);
    minitel.print("U");
    minitel.attributs(FOND_NORMAL);
    minitel.graphicMode();
    minitel.writeByte(0x35);
    minitel.textMode();
    minitel.print("SSH User: ");
    minitel.attributs(CARACTERE_CYAN);
    minitel.println("ssh username");
    minitel.attributs(CARACTERE_BLANC);
    minitel.graphicMode();
    minitel.writeByte(0x6A);
    minitel.textMode();
    minitel.attributs(INVERSION_FOND);
    minitel.print("P");
    minitel.attributs(FOND_NORMAL);
    minitel.graphicMode();
    minitel.writeByte(0x35);
    minitel.textMode();
    minitel.print("SSH Pass: ");
    minitel.attributs(CARACTERE_CYAN);
    minitel.println("ssh password");
    minitel.newXY(1, 24);
    minitel.attributs(CARACTERE_VERT);
    minitel.print("        ESC or SUMMARY to go back");

    uint32_t key;
    do
    {
        while ((key = minitel.getKeyCode()) == 0)
        {
        }
        if (key == 27 || key == 4933 || key == 4934)
        {
            break;
        }

        if (key == 18)
        {
            // CTRL+R = RESET
            //reset();
        }
    }
    while (true);

    showPrefs();
}

int manageHttpConnection()
{
    String header = "";
    String postBody = "";

    if (serverStatus != HTTP_SERVER_READY) return 0;

    WiFiClient client = server.available();
    if (!client) return 0;

    serverStatus = HTTP_SERVER_CLIENT_CONNECTED;
    debugPort.println("New Client.");
    String currentLine = "";
    int contentLength = -1;
    while (client.connected())
    {
        if (serverStatus != HTTP_SERVER_CLIENT_CONNECTED) break;
        if (client.available())
        {
            char c = client.read();
            header += c;
            if (header.length() > HTTP_SERVER_HEADER_MAX_LENGTH - 1)
            {
                debugPort.println("header too big");
                serverStatus = HTTP_SERVER_HEADER_OVERSIZED;
            }

            if (c == '\n')
            {
                if (currentLine.startsWith("Content-Length:"))
                {
                    contentLength = currentLine.substring(16).toInt();
                }
                if (contentLength > HTTP_SERVER_BODY_MAX_LENGTH)
                {
                    debugPort.println("body too big");
                    serverStatus = HTTP_SERVER_BODY_OVERSIZED;
                }
                if (currentLine.length() == 0)
                {
                    if (header.indexOf("POST") >= 0)
                    {
                        // Inizia a leggere il corpo del POST
                        while (postBody.length() < contentLength)
                        {
                            if (client.available())
                            {
                                postBody += client.read();
                            }
                        }
                        serverStatus = HTTP_SERVER_BODY_COMPLETE;
                    }
                    else if (header.indexOf("GET") >= 0)
                    {
                        serverStatus = HTTP_SERVER_GET_RECEIVED;
                    }
                    else
                    {
                        serverStatus = HTTP_SERVER_UNKNOWN_METHOD;
                    }
                }
                else
                {
                    currentLine = "";
                }
            }
            else if (c != '\r')
            {
                currentLine += c;
            }
        }
    }
    client.println("HTTP/1.1 200 OK");
    client.println("Content-type: text/html");
    client.println("Connection: close");
    client.println();
    client.println("<!DOCTYPE HTML><html>");
    client.println("<head><title>ESP32 Web Server</title></head><body>");
    if (serverStatus == HTTP_SERVER_HEADER_OVERSIZED)
    {
        client.println("<h2>Header is too big (" + String(HTTP_SERVER_HEADER_MAX_LENGTH) + " bytes max)</h2>");
    }
    if (serverStatus == HTTP_SERVER_BODY_OVERSIZED)
    {
        client.println("<h2>Body is too big (" + String(HTTP_SERVER_BODY_MAX_LENGTH) + " bytes max)</h2>");
    }
    if (serverStatus == HTTP_SERVER_BODY_COMPLETE)
    {
        client.println("<h2>Received POST data:</h2>");
        client.println("<pre>");
        client.println("Private Key Injected");
        client.println("</pre>");
        privKey = true;
        sshPrivKey = postBody;
        showPrefs();
    }
    else if (serverStatus == HTTP_SERVER_GET_RECEIVED)
    {
        client.println("<h2>GET request received</h2>");
    }
    client.println("</body></html>");
    client.println();

    client.stop();
    debugPort.println("Client disconnected.");
    debugPort.println("");
    return (serverStatus == HTTP_SERVER_BODY_COMPLETE);
}
