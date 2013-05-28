#include "rfb.h"
#include "htif.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <cstring>
#include <cinttypes>
using namespace std::placeholders;

rfb_t::rfb_t(int display)
  : memif(0), addr(0), width(0), height(0), bpp(0), display(display), fb(0),
    thread(0), nticks(0)
{
  register_command(0, std::bind(&rfb_t::handle_configure, this, _1), "configure");
  register_command(1, std::bind(&rfb_t::handle_set_address, this, _1), "set_address");
}

void rfb_t::thread_main()
{
  int port = 5900 + display;
  sockfd = socket(PF_INET, SOCK_STREAM, 0);
  if (sockfd < 0)
    throw std::runtime_error("could not acquire tcp socket");

  struct sockaddr_in saddr, caddr;
  saddr.sin_family = AF_INET;
  saddr.sin_addr.s_addr = INADDR_ANY;
  saddr.sin_port = htons(port);
  if (bind(sockfd, (struct sockaddr*)&saddr, sizeof(saddr)) < 0)
    throw std::runtime_error("could not bind to port " + std::to_string(port));
  if (listen(sockfd, 0) < 0)
    throw std::runtime_error("could not listen on port " + std::to_string(port));
 
  socklen_t clen = sizeof(caddr);
  afd = accept(sockfd, (struct sockaddr*)&caddr, &clen);
  if (afd < 0)
    throw std::runtime_error("could not accept connection");

  std::string version = "RFB 003.003\n";
  write(version);
  if (read() != version)
    throw std::runtime_error("bad client version");

  write(str(htonl(1)));

  read(); // clientinit

  std::string serverinit;
  serverinit += str(htons(width));
  serverinit += str(htons(height));
  serverinit += pixel_format();
  std::string name = "RISC-V";
  serverinit += str(htonl(name.length()));
  serverinit += name;
  write(serverinit);

  while (addr == 0);
    std::this_thread::yield();

  while (addr != 0)
  {
    printf("spin\n");
    std::string s = read();
    if (s.length() < 4)
      break; //throw std::runtime_error("bad command");

    switch (s[0])
    {
      case 0: set_pixel_format(s); break;
      case 2: set_encodings(s); break;
      case 3: fb_update(s); break;
    }
  }

  close(afd);
  close(sockfd);
}

rfb_t::~rfb_t()
{
  addr = 0;
  if (thread)
  {
    thread->join();
    delete thread;
  }
  delete [] fb;
}

void rfb_t::set_encodings(const std::string& s)
{
  uint16_t n = htons(*(uint16_t*)&s[2]);
  for (size_t b = s.length(); b < 4U+4U*n; b += read().length());
}

void rfb_t::set_pixel_format(const std::string& s)
{
  if (s.length() != 20 || s.substr(4, 16) != pixel_format())
    throw std::runtime_error("bad pixel format");
}

void rfb_t::fb_update(const std::string& s)
{
  std::string u;
  u += str(uint8_t(0));
  u += str(uint8_t(0));
  u += str(htons(1));
  u += str(htons(0));
  u += str(htons(0));
  u += str(htons(width));
  u += str(htons(height));
  u += str(htonl(0));
  u += std::string((char*)fb, fb_bytes());
  write(u);
}

void rfb_t::tick()
{
  if (!(addr && fb_bytes()))
    return;
  if (nticks++ % 100 == 0)
  {
    printf("htif\n");
    memif->read(addr, fb_bytes(), const_cast<char*>(fb));
  }
}

std::string rfb_t::pixel_format()
{
  int red_bits = 8, green_bits = 8, blue_bits = 8;
  int bpp = red_bits + green_bits + blue_bits;
  while (bpp & (bpp-1)) bpp++;

  std::string fmt;
  fmt += str(uint8_t(bpp));
  fmt += str(uint8_t(red_bits + green_bits + blue_bits));
  fmt += str(uint8_t(0)); // little-endian
  fmt += str(uint8_t(1)); // true color
  fmt += str(uint16_t((1<<red_bits)-1));
  fmt += str(uint16_t((1<<green_bits)-1));
  fmt += str(uint16_t((1<<blue_bits)-1));
  fmt += str(uint8_t(0));
  fmt += str(uint8_t(red_bits));
  fmt += str(uint8_t(red_bits+green_bits));
  fmt += str(uint16_t(0)); // pad
  fmt += str(uint8_t(0)); // pad
  return fmt;
}

void rfb_t::write(const std::string& s)
{
  if ((size_t)::write(afd, s.c_str(), s.length()) != s.length())
    throw std::runtime_error("could not write");
}

std::string rfb_t::read()
{
  char buf[2048];
  ssize_t len = ::read(afd, buf, sizeof(buf));
  if (len < 0)
    throw std::runtime_error("could not read");
  if (len == sizeof(buf))
    throw std::runtime_error("received oversized packet");
  return std::string(buf, len);
}

void rfb_t::handle_configure(command_t cmd)
{
  if (width || height || bpp)
    throw std::runtime_error("you must only set the rfb configuration once");

  width = cmd.payload();
  height = cmd.payload() >> 16;

  bpp = cmd.payload() >> 32;
  if (bpp != 32)
    throw std::runtime_error("rfb requires 32 bpp true color");

  fb = new char[fb_bytes()];
  thread = new std::thread(&rfb_t::thread_main, this);
  cmd.respond(1);
}

void rfb_t::handle_set_address(command_t cmd)
{
  addr = cmd.payload();
  memif = &cmd.htif()->memif();
  cmd.respond(1);
}
