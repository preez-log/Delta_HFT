#pragma once
#include <iostream>
#include <string>
#include <vector>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <random>
#include <mutex>
#include <cerrno>
#include "../core/Logger.h"

template <typename Derived>
class SocketBase {
protected:
    SSL_CTX* ctx = nullptr;
    SSL* ssl = nullptr;
    int sock = -1;
    bool connected = false;
    
    std::string host;
    int port;
    
    std::vector<char> buffer_;
    char read_buffer[1024 * 16]; // 16KB 버퍼
    
    std::recursive_mutex ssl_mtx_;

public:
    SocketBase(const std::string& h, int p = 443) : host(h), port(p) {
        init_openssl();
        buffer_.reserve(1024 * 64);
    }

    ~SocketBase() { cleanup(); }

    bool connect(const std::string& path = "/ws") {
        std::lock_guard<std::recursive_mutex> lock(ssl_mtx_);
        cleanup(); 
        buffer_.clear();
        
        struct hostent* host_entry = gethostbyname(host.c_str());
        if (!host_entry) {
            LOG(">>> [socket] DNS Failed: %s", host.c_str());
            return false;
        }

        sock = socket(AF_INET, SOCK_STREAM, 0);
        
        int flag = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(int));
        
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr = *((struct in_addr*)host_entry->h_addr);

        if (::connect(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
            return false;
        }

        ssl = SSL_new(ctx);
        SSL_set_fd(ssl, sock);
        SSL_set_tlsext_host_name(ssl, host.c_str());

        if (SSL_connect(ssl) != 1) {
            LOG(">>> [socket] SSL Handshake Failed");
            return false;
        }

        if (!ws_handshake(path)) {
            return false; 
        }

        // Non-blocking 설정
        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        connected = true;
        return true;
    }

    void poll() {
        std::lock_guard<std::recursive_mutex> lock(ssl_mtx_);
        if (!connected || !ssl) return;

        // Non-blocking 읽기
        int bytes_read = SSL_read(ssl, read_buffer, sizeof(read_buffer));
        
        if (bytes_read > 0) {
            buffer_.insert(buffer_.end(), read_buffer, read_buffer + bytes_read);
        }
        else {
            int err = SSL_get_error(ssl, bytes_read);
            
            // 정상상태
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                goto PARSE_BUFFER;
            }
            if (err == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                goto PARSE_BUFFER;
            }
            
            // 진짜 에러인 경우에만 로그 찍고 종료
            LOG(">>> [socket] SSL Error: %d, Errno: %d. Closing.", err, errno);
            cleanup(); 
            return;
        }
    PARSE_BUFFER:    
        size_t processed_offset = 0;
        
        // 패킷 파싱 로직
        while (buffer_.size() - processed_offset >= 2) {
            unsigned char* ptr = (unsigned char*)buffer_.data() + processed_offset;
            size_t header_len = 2;
            size_t payload_len = ptr[1] & 0x7F;

            if (payload_len == 126) {
                if (buffer_.size() - processed_offset < 4) break; 
                payload_len = (ptr[2] << 8) | ptr[3];
                header_len = 4;
            } else if (payload_len == 127) {
                if (buffer_.size() - processed_offset < 10) break; 
                payload_len = (size_t)ptr[9] | ((size_t)ptr[8] << 8) | ((size_t)ptr[7] << 16) | ((size_t)ptr[6] << 24); 
                header_len = 10;
            }

            if (buffer_.size() - processed_offset >= header_len + payload_len) {
                unsigned char opcode = ptr[0] & 0x0F;

                if (opcode == 0x09) { // Ping
                    std::string ping_payload((char*)(ptr + header_len), payload_len);
                    send_frame(ping_payload, 0x8A);
                }
                else if (opcode == 0x08) { // Close
                    unsigned short close_code = 0;
                    std::string reason = "";
                    
                    if (payload_len >= 2) {
                        unsigned char* p = (unsigned char*)(ptr + header_len);
                        close_code = (p[0] << 8) | p[1]; // Big Endian
                        if (payload_len > 2) {
                            reason = std::string((char*)p + 2, payload_len - 2);
                        }
                    }

                    if (close_code != 1000) {
                        LOG(">>> [socket] Server Closed Connection! Code: %d, Reason: %s", close_code, reason.c_str());
                    }
                    cleanup();
                    return;
                }
                else if (opcode == 0x01 || opcode == 0x02) { 
                    char* payload = (char*)(ptr + header_len);
                    static_cast<Derived*>(this)->on_message(payload, payload_len);
                }

                processed_offset += header_len + payload_len;
            } 
            else {
                break;
            }
        }
        
        if (processed_offset > 0) {
            buffer_.erase(buffer_.begin(), buffer_.begin() + processed_offset);
        }
    }

    void send(const std::string& data) {
        std::lock_guard<std::recursive_mutex> lock(ssl_mtx_);
        if (connected && ssl) {
            send_frame(data, 0x81); // 0x81 = Text Frame
        }
    }
    
    void send_ping(const std::string& data = "") {
        std::lock_guard<std::recursive_mutex> lock(ssl_mtx_);
        if (connected && ssl) {
            send_frame(data, 0x89); // 0x89 = Ping Frame
        }
    }

    bool is_connected() const { return connected; }

protected:

    void send_frame(const std::string& data, unsigned char opcode) {
        std::vector<unsigned char> frame;
        frame.push_back(opcode); 

        size_t len = data.size();
        if (len <= 125) {
            frame.push_back(0x80 | len);
        } else if (len <= 65535) {
            frame.push_back(0x80 | 126);
            frame.push_back((len >> 8) & 0xFF);
            frame.push_back(len & 0xFF);
        } else {
            frame.push_back(0x80 | 127);
            for (int i = 0; i < 4; ++i) frame.push_back(0); 
            frame.push_back((len >> 24) & 0xFF);
            frame.push_back((len >> 16) & 0xFF);
            frame.push_back((len >> 8) & 0xFF);
            frame.push_back(len & 0xFF);
        }

        static uint32_t fast_seed = 123456789;
        fast_seed ^= fast_seed << 13;
        fast_seed ^= fast_seed >> 17;
        fast_seed ^= fast_seed << 5;
        
        unsigned char mask[4];
        std::memcpy(mask, &fast_seed, 4);

        frame.insert(frame.end(), mask, mask+4);

        for (size_t i = 0; i < len; ++i) {
            frame.push_back(data[i] ^ mask[i % 4]);
        }
        SSL_write(ssl, frame.data(), frame.size());
    }

    bool ws_handshake(const std::string& path) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 255);

        unsigned char rnd[16];
        for(int i=0; i<16; ++i) rnd[i] = (unsigned char)dis(gen);

        std::string key;
        const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        
        for(int i=0; i<16; i+=3) {
            // 3바이트씩 묶어서 처리
            unsigned int n = (rnd[i] << 16) + ( (i+1<16 ? rnd[i+1] : 0) << 8 ) + (i+2<16 ? rnd[i+2] : 0);
            
            key += b64[(n >> 18) & 63];
            key += b64[(n >> 12) & 63];
            
            // 패딩 처리
            if(i+1 < 16) key += b64[(n >> 6) & 63];
            else key += '=';
            
            if(i+2 < 16) key += b64[n & 63];
            else key += '=';
        }

        // 헤더 생성
        std::string req = "GET " + path + " HTTP/1.1\r\n"
                          "Host: " + host + "\r\n"
                          "Upgrade: websocket\r\n"
                          "Connection: Upgrade\r\n"
                          "Sec-WebSocket-Key: " + key + "\r\n"
                          "Sec-WebSocket-Version: 13\r\n"
                          "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.124 Safari/537.36\r\n" 
                          "Origin: https://www.binance.com\r\n"
                          "\r\n";
        
        if (SSL_write(ssl, req.c_str(), req.length()) <= 0) return false;

        char buf[4096];
        int n = SSL_read(ssl, buf, sizeof(buf) - 1);
        if (n <= 0) return false;

        if (strstr(buf, "101 Switching Protocols") == NULL) {
            buf[n] = 0;
            LOG(">>> [socket] Handshake Failed: %s", buf);
            return false;
        }
        if (n > 200) {
        }
        
        return true;
    }

    void init_openssl() {
        if (!ctx) {
            SSL_library_init();
            OpenSSL_add_all_algorithms();
            SSL_load_error_strings();
            ctx = SSL_CTX_new(TLS_client_method());
        }
    }

    void cleanup() {
        if (ssl) { SSL_free(ssl); ssl = nullptr; }
        if (sock != -1) { close(sock); sock = -1; }
        connected = false;
        buffer_.clear();
    }
};