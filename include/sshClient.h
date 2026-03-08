// source: https://github.com/jbellue/3615_SSH

#ifndef MINITEL_TELNET_PRO_SSHCLIENT_H
#define MINITEL_TELNET_PRO_SSHCLIENT_H

#include <libssh/libssh.h>

class SSHClient {
public:
    SSHClient();

    enum class SSHStatus {
        OK,
        AUTHENTICATION_ERROR,
        GENERAL_ERROR
    };

    bool begin(const char *host, int port, const char *user, const char *password, bool privKey, const char *sshPrivKey);
    SSHStatus connect_ssh(const char *host, int port, const char *user, const char *password, bool privKey, const char *sshPrivKey, int verbosity);
    SSHStatus start_session(const char *host, int port, const char *user, const char *password, bool privKey, const char *sshPrivKey);
    void close_session() const;
    int interactive_shell_session() const;
    void close_channel() const;
    bool open_channel();
    void end() const;
    bool available() const;
    int receive();
    int flushReceiving();
    char readIndex(int index) const;
    int send(const void *buffer, uint32_t len) const;

private:
    ssh_session _session{};
    ssh_channel _channel{};
    ssh_key privkey = nullptr;

    char _readBuffer[256] = {'\0'};
};

#endif //MINITEL_TELNET_PRO_SSHCLIENT_H