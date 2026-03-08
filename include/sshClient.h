// source: https://github.com/jbellue/3615_SSH

#ifndef MINITEL_TELNET_PRO_SSHCLIENT_H
#define MINITEL_TELNET_PRO_SSHCLIENT_H

#include <libssh_esp32.h>
#include <libssh/libssh.h>
#include <string.h>

class SSHClient {
public:
    SSHClient();

    enum class SSHStatus {
        OK,
        AUTHENTICATION_ERROR,
        GENERAL_ERROR
    };

    bool begin(const char *host, int port, const char *user, const char *password, bool privKey, const char *sshPrivKey);
    SSHStatus connect_ssh(const char *host, const int port, const char *user, const char *password, bool privKey, const char *sshPrivKey, const int verbosity);
    //    bool poll(Minitel* minitel);
    SSHStatus start_session(const char *host, const int port, const char *user, const char *password, bool privKey, const char *sshPrivKey);
    void close_session() const;
    int interactive_shell_session();
    void close_channel();
    bool open_channel();
    void end();
    bool available() const;
    int receive();
    int flushReceiving();
    char readIndex(int index) const;
    int send(void *buffer, uint32_t len) const;

private:
    ssh_session _session;
    ssh_channel _channel;
    ssh_key privkey = nullptr;

    char _readBuffer[256] = {'\0'};
};

#endif //MINITEL_TELNET_PRO_SSHCLIENT_H