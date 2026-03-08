// source: https://github.com/jbellue/3615_SSH

#include <cstring>
#include <libssh_esp32.h>
#include <libssh/libssh.h>
#include "sshClient.h"

SSHClient::SSHClient() = default;

bool SSHClient::begin(const char* host, const int port, const char* username, const char* password, bool isPrivKey,
                      const char* sshPrivKey)
{
    privkey = nullptr;
    libssh_begin();
    const SSHStatus status = start_session(host, port, username, password, isPrivKey, sshPrivKey);
    if (status != SSHStatus::OK || !open_channel() || SSH_OK != interactive_shell_session())
    {
        return false;
    }

    return true;
}

bool SSHClient::available() const
{
    if (!ssh_channel_is_open(_channel) || ssh_channel_is_eof(_channel))
    {
        return false;
    }
    return true;
}

int SSHClient::receive()
{
    memset(_readBuffer, '\0', sizeof(_readBuffer));
    return ssh_channel_read_nonblocking(_channel, _readBuffer, sizeof(_readBuffer), 0);
}

int SSHClient::flushReceiving()
{
    int tbyte = 0;
    uint8_t count = 0;
    do
    {
        int nbyte = ssh_channel_read_nonblocking(_channel, _readBuffer, sizeof(_readBuffer), 0);
        tbyte += nbyte;
        if (nbyte == 0)
        {
            vTaskDelay(100 / portTICK_PERIOD_MS);
            count++;
        }
    }
    while (count < 3);
    return tbyte;
}

char SSHClient::readIndex(int index) const
{
    return _readBuffer[index];
}

int SSHClient::send(const void* buffer, const uint32_t len) const
{
    const int rc = ssh_channel_write(_channel, buffer, len);
    if (rc == SSH_ERROR) return -1;
    return rc;
}

void SSHClient::end() const
{
    close_channel();
    close_session();
}

SSHClient::SSHStatus SSHClient::connect_ssh(const char* host, const int port, const char* user, const char* password,
                                            bool isPrivKey, const char* sshPrivKey, const int verbosity)
{
    _session = ssh_new();

    if (_session == nullptr)
    {
        return SSHStatus::GENERAL_ERROR;
    }

    if (ssh_options_set(_session, SSH_OPTIONS_USER, user) < 0)
    {
        ssh_free(_session);
        return SSHStatus::GENERAL_ERROR;
    }

    if (ssh_options_set(_session, SSH_OPTIONS_HOST, host) < 0)
    {
        ssh_free(_session);
        return SSHStatus::GENERAL_ERROR;
    }

    // https://github.com/ewpa/LibSSH-ESP32/blob/master/src/libssh/libssh.h#L376
    if (ssh_options_set(_session, SSH_OPTIONS_PORT, &port) < 0)
    {
        ssh_free(_session);
        return SSHStatus::GENERAL_ERROR;
    }

    ssh_options_set(_session, SSH_OPTIONS_LOG_VERBOSITY, &verbosity);

    if (ssh_connect(_session))
    {
        ssh_disconnect(_session);
        ssh_free(_session);
        return SSHStatus::GENERAL_ERROR;
    }

    if (isPrivKey)
    {
        int rc = ssh_pki_import_privkey_base64(sshPrivKey, nullptr, nullptr, nullptr, &privkey);
        if (rc != SSH_OK)
        {
            ssh_disconnect(_session);
            ssh_free(_session);
            return SSHStatus::GENERAL_ERROR;
        }

        rc = ssh_userauth_publickey(_session, nullptr, privkey);
        if (rc != SSH_AUTH_SUCCESS)
        {
            ssh_key_free(privkey);
            ssh_disconnect(_session);
            ssh_free(_session);
            return SSHStatus::GENERAL_ERROR;
        }
    }
    else
    {
        // USER AND PASSWORD
        // Authenticate ourselves
        if (ssh_userauth_password(_session, nullptr, password) != SSH_AUTH_SUCCESS)
        {
            ssh_disconnect(_session);
            ssh_free(_session);
            return SSHStatus::AUTHENTICATION_ERROR;
        }
    }
    return SSHStatus::OK;
}


bool SSHClient::open_channel()
{
    _channel = ssh_channel_new(_session);
    if (_channel == nullptr)
    {
        return false;
    }
    const int ret = ssh_channel_open_session(_channel);
    if (ret != SSH_OK)
    {
        return false;
    }
    return true;
}

void SSHClient::close_channel() const
{
    if (_channel != nullptr)
    {
        ssh_channel_close(_channel);
        ssh_channel_send_eof(_channel);
        ssh_channel_free(_channel);
    }
}

int SSHClient::interactive_shell_session() const
{
    int ret = ssh_channel_request_pty_size(_channel, "vt100", 80, 24);
    if (ret != SSH_OK)
    {
        return ret;
    }

    ret = ssh_channel_request_shell(_channel);
    if (ret != SSH_OK)
    {
        return ret;
    }

    return ret;
}

SSHClient::SSHStatus SSHClient::start_session(const char* host, const int port, const char* user, const char* password,
                                              bool isPrivKey, const char* sshPrivKey)
{
    const SSHStatus status = connect_ssh(host, port, user, password, isPrivKey, sshPrivKey, SSH_LOG_NOLOG);
    if (status != SSHStatus::OK)
    {
        ssh_finalize();
    }
    return status;
}

void SSHClient::close_session() const
{
    if (_session != nullptr)
    {
        if (privkey != nullptr) ssh_key_free(privkey);
        ssh_disconnect(_session);
        ssh_free(_session);
    }
}
