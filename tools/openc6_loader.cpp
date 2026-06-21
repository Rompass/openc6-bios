#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <iomanip>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <algorithm>

constexpr uint8_t CMD_ACK = 0x06;
constexpr uint8_t CMD_EOT = 0x04;
constexpr uint8_t CMD_NAK = 0x15;
constexpr size_t  CHUNK_SIZE = 64;

class SerialPort {
private:
    int fd = -1;
public:
    SerialPort(const std::string& port_name) {
        fd = open(port_name.c_str(), O_RDWR | O_NOCTTY | O_SYNC);
        if (fd < 0) throw std::runtime_error("Failed to open port");

        struct termios tty;
        if (tcgetattr(fd, &tty) != 0) throw std::runtime_error("Error from tcgetattr");

        cfsetospeed(&tty, B115200);
        cfsetispeed(&tty, B115200);

        tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
        tty.c_iflag &= ~IGNBRK;
        tty.c_lflag = 0;
        tty.c_oflag = 0;
        tty.c_cc[VMIN]  = 0;
        tty.c_cc[VTIME] = 1;

        tty.c_iflag &= ~(IXON | IXOFF | IXANY);
        tty.c_cflag |= (CLOCAL | CREAD);
        tty.c_cflag &= ~(PARENB | PARODD);
        tty.c_cflag &= ~CSTOPB;
        tty.c_cflag &= ~CRTSCTS;

        if (tcsetattr(fd, TCSANOW, &tty) != 0) throw std::runtime_error("Error from tcsetattr");
    }

    ~SerialPort() { if (fd >= 0) close(fd); }
    int get_fd() const { return fd; }

    bool write_data(const void* data, size_t len) {
        size_t written = 0;
        const uint8_t* ptr = (const uint8_t*)data;
        while (written < len) {
            ssize_t res = write(fd, ptr + written, len - written);
            if (res < 0) return false;
            written += res;
        }
        return true;
    }

    bool read_byte(uint8_t& byte) {
        ssize_t res = read(fd, &byte, 1);
        return (res == 1);
    }
};

void draw_progress(size_t current, size_t total) {
    int barWidth = 50;
    float progress = (float)current / total;
    int pos = barWidth * progress;
    std::cout << "[";
    for (int i = 0; i < barWidth; ++i) {
        if (i < pos) std::cout << "=";
        else if (i == pos) std::cout << ">";
        else std::cout << " ";
    }
    std::cout << "] " << int(progress * 100.0) << " %\r";
    std::cout.flush();
}

int main(int argc, char* argv[]) {
    std::cout << "==========================================\n";
    std::cout << " OpenC6 BIOS - Payload Loader (Robust Sync)\n";
    std::cout << "==========================================\n";

    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <serial_port> <payload.bin>\n";
        return 1;
    }

    std::ifstream file(argv[2], std::ios::binary | std::ios::ate);
    if (!file) return 1;
    std::streamsize file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> payload(file_size);
    if (!file.read((char*)payload.data(), file_size)) return 1;

    try {
        SerialPort serial(argv[1]);
        tcflush(serial.get_fd(), TCIOFLUSH);

        std::cout << "Waiting for Text Marker '##OPENC6_SYNC##' from BIOS...\n";

        std::string buffer;
        bool synced = false;
        uint8_t rx_byte;
        auto start_time = std::chrono::steady_clock::now();

        while (std::chrono::steady_clock::now() - start_time < std::chrono::seconds(30)) {
            if (serial.read_byte(rx_byte)) {
                buffer += (char)rx_byte;
                if (buffer.find("##OPENC6_SYNC##") != std::string::npos) {
                    synced = true;
                    break;
                }
                if (buffer.size() > 512) buffer.erase(0, 256);
            }
        }

        if (!synced) {
            std::cerr << "\nERROR: Handshake Timeout! BIOS did not send SYNC marker.\n";
            return 1;
        }

        std::cout << "BIOS is ready! Sending file size with Magic Preambule...\n";

        uint8_t size_buf[6] = {
            0x5A, 0xA5,
            (uint8_t)(file_size & 0xFF),
            (uint8_t)((file_size >> 8) & 0xFF),
            (uint8_t)((file_size >> 16) & 0xFF),
            (uint8_t)((file_size >> 24) & 0xFF)
        };
        serial.write_data(size_buf, 6);

        std::cout << "Waiting for BIOS to allocate RAM or Erase Flash...\n";
        auto erase_wait_start = std::chrono::steady_clock::now();
        bool ack_received = false;

        while (std::chrono::steady_clock::now() - erase_wait_start < std::chrono::seconds(10)) {
            if (serial.read_byte(rx_byte)) {
                ack_received = true;
                break;
            }
        }

        if (!ack_received) {
            std::cerr << "ERROR: Timeout waiting for size ACK (Flash erase took too long?)!\n";
            return 1;
        }

        if (rx_byte == CMD_NAK) {
            std::cerr << "ERROR: BIOS rejected file size! (Out of IRAM or Flash?)\n";
            return 1;
        } else if (rx_byte != CMD_ACK) {
            std::cerr << "ERROR: Unexpected response: 0x" << std::hex << (int)rx_byte << "\n";
            return 1;
        }

        std::cout << "Flashing Payload...\n";
        size_t total_sent = 0;

        while (total_sent < file_size) {
            size_t chunk = std::min(CHUNK_SIZE, payload.size() - total_sent);

            serial.write_data(payload.data() + total_sent, chunk);

            if (!serial.read_byte(rx_byte) || rx_byte != CMD_ACK) {
                std::cerr << "\nERROR: Failed waiting for ACK at offset " << total_sent << "!\n";
                return 1;
            }

            total_sent += chunk;
            draw_progress(total_sent, file_size);
        }

        std::cout << "\nTransmission 100% Complete.\n";
        uint8_t eot = CMD_EOT;
        serial.write_data(&eot, 1);
        std::cout << "EOT sent. BIOS is jumping to Payload!\n";
        std::cout << "SUCCESS.\n";

    } catch (const std::exception& e) {
        std::cerr << "EXCEPTION: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
