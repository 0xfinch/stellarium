/*
 * Author and Copyright of this file and of the stellarium telescope feature:
 * Johannes Gajdosik, 2006
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#include "Telescope.hpp"
#include "StelUtils.hpp"
#include "Translator.hpp"
#include "StelCore.hpp"

#include <sstream>
#include <iomanip>
#include <math.h>

#include <QTextStream>
#include <QString>
#include <QStringList>
#include <QRegExp>
#include <QDebug>

#ifdef WIN32
  #include <windows.h> // GetSystemTimeAsFileTime
  #include <winsock2.h>
  #define ERRNO WSAGetLastError()
  #define STRERROR(x) x
  #undef EAGAIN
  #define EAGAIN WSAEWOULDBLOCK
  #undef EINTR
  #define EINTR WSAEINTR
  #undef EINPROGRESS
  #define EINPROGRESS WSAEINPROGRESS
  static u_long ioctlsocket_arg = 1;
  #define SET_NONBLOCKING_MODE(s) ioctlsocket(s,FIONBIO,&ioctlsocket_arg)
  #define SOCKLEN_T int
  #define close closesocket
  #define IS_INVALID_SOCKET(fd) (fd==INVALID_SOCKET)
#else
  #include <netdb.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  #include <string.h> // strerror
  #define ERRNO errno
  #define STRERROR(x) strerror(x)
  #define SET_NONBLOCKING_MODE(s) fcntl(s,F_SETFL,O_NONBLOCK)
  #define SOCKLEN_T socklen_t
  #define SOCKET int
  #define IS_INVALID_SOCKET(fd) (fd<0)
  #define INVALID_SOCKET (-1)
#endif

struct PrintRaDec {
  PrintRaDec(const unsigned int ra_int,const int dec_int)
    :ra_int(ra_int),dec_int(dec_int) {}
  const unsigned int ra_int;
  const int dec_int;
};

template<class T>
T &operator<<(T &o,const PrintRaDec &x) {
  unsigned int h = x.ra_int;
  int d = (int)floor(0.5+x.dec_int*(360*3600*1000/4294967296.0));
  char dec_sign;
  if (d >= 0) {
    if (d > 90*3600*1000) {
      d =  180*3600*1000 - d;
      h += 0x80000000;
    }
    dec_sign = '+';
  } else {
    if (d < -90*3600*1000) {
      d = -180*3600*1000 - d;
      h += 0x80000000;
    }
    d = -d;
    dec_sign = '-';
  }
  h = (unsigned int)floor(0.5+h*(24*3600*10000/4294967296.0));
  const int ra_ms = h % 10000; h /= 10000;
  const int ra_s = h % 60; h /= 60;
  const int ra_m = h % 60; h /= 60;
  h %= 24;
  const int dec_ms = d % 1000; d /= 1000;
  const int dec_s = d % 60; d /= 60;
  const int dec_m = d % 60; d /= 60;
  o << "ra = "
    << setfill(' ') << setw(2) << h << 'h'
    << setfill('0') << setw(2) << ra_m << 'm'
    << setfill('0') << setw(2) << ra_s << '.'
    << setfill('0') << setw(4) << ra_ms
    << " dec = "
    << ((d<10)?" ":"") << dec_sign << d << 'd'
    << setfill('0') << setw(2) << dec_m << 'm'
    << setfill('0') << setw(2) << dec_s << '.'
    << setfill('0') << setw(3) << dec_ms
    << setfill(' ');
  return o;
}

class TelescopeDummy : public Telescope {
public:
  TelescopeDummy(const QString &name,const QString &params) : Telescope(name) {
    desired_pos[0] = XYZ[0] = 1.0;
    desired_pos[1] = XYZ[1] = 0.0;
    desired_pos[2] = XYZ[2] = 0.0;
  }
private:
  bool isConnected(void) const {return true;}
  bool hasKnownPosition(void) const {return true;}
  Vec3d getObsJ2000Pos(const Navigator *nav=0) const {return XYZ;}
  void prepareSelectFds(fd_set&,fd_set&,int&) {
    XYZ = XYZ*31.0+desired_pos;
    const double lq = XYZ.lengthSquared();
    if (lq > 0.0) XYZ *= (1.0/sqrt(lq));
    else XYZ = desired_pos;
  }
  void telescopeGoto(const Vec3d &j2000_pos) {
    desired_pos = j2000_pos;
    desired_pos.normalize();
  }
  Vec3d XYZ; // j2000 position
  Vec3d desired_pos;
};


class TelescopeTcp : public Telescope {
public:
  TelescopeTcp(const QString &name,const QString &params);
  ~TelescopeTcp(void) {hangup();}
private:
  bool isConnected(void) const
    {return (!IS_INVALID_SOCKET(fd) && !wait_for_connection_establishment);}
  Vec3d getObsJ2000Pos(const Navigator *nav=0) const;
  void prepareSelectFds(fd_set &read_fds,fd_set &write_fds,int &fd_max);
  void handleSelectFds(const fd_set &read_fds,const fd_set &write_fds);
  void telescopeGoto(const Vec3d &j2000_pos);
  bool isInitialized(void) const {return (ntohs(address.sin_port)!=0);}
  void performReading(void);
  void performWriting(void);
private:
  void hangup(void);
  struct sockaddr_in address;
  SOCKET fd;
  bool wait_for_connection_establishment;
  long long int end_of_timeout;
  char read_buff[120];
  char *read_buff_end;
  char write_buff[120];
  char *write_buff_end;
  int time_delay;
  struct Position {
    long long int server_micros;
    long long int client_micros;
    Vec3d pos;
    int status;
  };
  Position positions[16];
  Position *position_pointer;
  Position *const end_position;
  virtual bool hasKnownPosition(void) const
    {return (position_pointer->client_micros!=0x7FFFFFFFFFFFFFFFLL);}
};

Telescope *Telescope::create(const QString &url) {
  // example url: My_first_telescope:TCP:localhost:10000:500000
  // split to:
  // name    = My_first_telescope
  // type    = TCP
  // params  = localhost:10000:500000
  //
  // The params part is optional.  We will use QRegExp to validate
  // the url and extact the components.

  // note, in a reg exp, [^:] matches any chararacter except ':'
  QRegExp recRx("^([^:]*):([^:]*)(:(.*))?$");
  QString name, type, params;
  if (recRx.exactMatch(url))
  {
    // trimmed removes whitespace on either end of a QString
    name = recRx.capturedTexts().at(1).trimmed();
    type = recRx.capturedTexts().at(2).trimmed();
    params = recRx.capturedTexts().at(4).trimmed();
  }
  else
  {
    qWarning() << "WARNING - telescope definition" << url << "not recognised";
    return NULL;
  }

  qDebug() << "Creating telescope" << url 
           << "; name/type/params:" << name 
           << type << params;

  Telescope *rval = 0;
  if (type == "Dummy") {
    rval = new TelescopeDummy(name,params);
  } else if (type == "TCP") {
    rval = new TelescopeTcp(name,params);
  } else {
    qWarning() << "WARNING - unknown telescope type" << type << "- not creating a telescope object for url" << url;
  }
  if (rval && !rval->isInitialized()) {
    delete rval;
    rval = 0;
  }
  return rval;
}


Telescope::Telescope(const QString &name) : name(name)
{
	nameI18n = name;
}

QString Telescope::getInfoString(const StelCore* core, const InfoStringGroup& flags) const
{
	const Navigator* nav = core->getNavigation();
	const Vec3d j2000_pos = getObsJ2000Pos(nav);
	double dec_j2000, ra_j2000;
	StelUtils::rect_to_sphe(&ra_j2000,&dec_j2000,j2000_pos);
	const Vec3d equatorial_pos = nav->j2000ToEarthEqu(j2000_pos);
	double dec_equ, ra_equ;
	StelUtils::rect_to_sphe(&ra_equ,&dec_equ,equatorial_pos);
	QString str;
	QTextStream oss(&str);
	if (flags&Name)
	{
		if (!(flags&PlainText))
			oss << QString("<font color=%1>").arg(StelUtils::vec3fToHtmlColor(getInfoColor()));

		oss << "<h2>" << nameI18n << "</h2>";
	}

	if (flags&RaDecJ2000) 
		oss << q_("J2000 RA/DE: %1/%2").arg(StelUtils::radToHmsStr(ra_j2000,false), StelUtils::radToDmsStr(dec_j2000,false)) << "<br>";

	if (flags&RaDec)
		oss << q_("Equ of date RA/DE: %1/%2").arg(StelUtils::radToHmsStr(ra_equ), StelUtils::radToDmsStr(dec_equ));

	// chomp trailing line breaks
	str.replace(QRegExp("<br(\\s*/)?>\\s*$"), "");

	if (flags&PlainText)
	{
		str.replace("<b>", "");
		str.replace("</b>", "");
		str.replace("<h2>", "");
		str.replace("</h2>", "\n");
		str.replace("<br>", "\n");
	}

	return str;
}

long long int GetNow(void) {
#ifdef WIN32
  FILETIME file_time;
  GetSystemTimeAsFileTime(&file_time);
  return (*((__int64*)(&file_time))/10) - 86400000000LL*134774;
#else
  struct timeval tv;
  gettimeofday(&tv,0);
  return tv.tv_sec * 1000000LL + tv.tv_usec;
#endif
}

TelescopeTcp::TelescopeTcp(const QString &name,const QString &params)
  : Telescope(name),fd(INVALID_SOCKET),
    end_position(positions+(sizeof(positions)/sizeof(positions[0]))) {
  hangup();
  address.sin_port = htons(0);

  // Example params:
  // localhost:10000:500000
  // split into:
  // host       = localhost
  // port       = 10000 (int)
  // time_delay = 500000 (int)

  QRegExp paramRx("^([^:]*):(\\d+):(\\d+)$");
  QString host;
  int port;
  if (paramRx.exactMatch(params))
  {
    // I will not use the ok param to toInt as the 
    // QRegExp only matches valid integers.
    host       = paramRx.capturedTexts().at(1).trimmed();
    port       = paramRx.capturedTexts().at(2).toInt();
    time_delay = paramRx.capturedTexts().at(3).toInt();
  }
  else
  {
    qWarning() << "WARNING - incorrect TelescopeTcp parameters";
    return;
  }

  qDebug() << "TelescopeTcp paramaters host, port, time_delay:" << host << port << time_delay;

  if (port<=0 || port>0xFFFF) {
    qWarning() << "ERROR creating TelescopeTcp - port not valid (should be less than 32767)";
    return;
  }
  if (time_delay<=0 || time_delay>10000000) {
    qWarning() << "ERROR creating TelescopeTcp - time_delay not valid (should be less than 10000000)";
    return;
  }
  struct hostent *hep = gethostbyname(host.toLocal8Bit());
  if (hep == 0) {
    qDebug() << "ERROR creating TelescopeTcp - unknown host" << host;
    return;
  }
  if (hep->h_length != 4) {
    qDebug() << "ERROR creating TelescopeTcp - host address is not IPv4";
    return;
  }
  memset(&address,0,sizeof(struct sockaddr_in));
  memcpy(&(address.sin_addr),hep->h_addr,4);
  address.sin_port = htons(port);
  address.sin_family = AF_INET;
  end_of_timeout = -0x8000000000000000LL;

  for (position_pointer = positions;
       position_pointer < end_position;
       position_pointer++) {
    position_pointer->server_micros = 0x7FFFFFFFFFFFFFFFLL;
    position_pointer->client_micros = 0x7FFFFFFFFFFFFFFFLL;
    position_pointer->pos[0] = 0.0;
    position_pointer->pos[1] = 0.0;
    position_pointer->pos[2] = 0.0;
    position_pointer->status = 0;
  }
  position_pointer = positions;
}

void TelescopeTcp::hangup(void) {
  if (!IS_INVALID_SOCKET(fd)) {
    close(fd);
    fd = INVALID_SOCKET;
  }
  read_buff_end = read_buff;
  write_buff_end = write_buff;
  wait_for_connection_establishment = false;
  for (position_pointer = positions;
       position_pointer < end_position;
       position_pointer++) {
    position_pointer->server_micros = 0x7FFFFFFFFFFFFFFFLL;
    position_pointer->client_micros = 0x7FFFFFFFFFFFFFFFLL;
    position_pointer->pos[0] = 0.0;
    position_pointer->pos[1] = 0.0;
    position_pointer->pos[2] = 0.0;
    position_pointer->status = 0;
  }
  position_pointer = positions;
}

void TelescopeTcp::telescopeGoto(const Vec3d &j2000_pos) {
  if (isConnected()) {
    if (write_buff_end-write_buff+20 < (int)sizeof(write_buff)) {
      const double ra = atan2(j2000_pos[1],j2000_pos[0]);
      const double dec = atan2(j2000_pos[2],
              sqrt(j2000_pos[0]*j2000_pos[0]+j2000_pos[1]*j2000_pos[1]));
      unsigned int ra_int = (unsigned int)floor(
                               0.5 +  ra*(((unsigned int)0x80000000)/M_PI));
      int dec_int = (int)floor(0.5 + dec*(((unsigned int)0x80000000)/M_PI));
//      qDebug() << "TelescopeTcp(" << name << ")::telescopeGoto: "
//              "queuing packet: " << PrintRaDec(ra_int,dec_int);
        // length of packet:
      *write_buff_end++ = 20;
      *write_buff_end++ = 0;
        // type of packet:
      *write_buff_end++ = 0;
      *write_buff_end++ = 0;
        // client_micros:
      long long int now = GetNow();
      *write_buff_end++ = now;now>>=8;
      *write_buff_end++ = now;now>>=8;
      *write_buff_end++ = now;now>>=8;
      *write_buff_end++ = now;now>>=8;
      *write_buff_end++ = now;now>>=8;
      *write_buff_end++ = now;now>>=8;
      *write_buff_end++ = now;now>>=8;
      *write_buff_end++ = now;
        // ra:
      *write_buff_end++ = ra_int;ra_int>>=8;
      *write_buff_end++ = ra_int;ra_int>>=8;
      *write_buff_end++ = ra_int;ra_int>>=8;
      *write_buff_end++ = ra_int;
        // dec:
      *write_buff_end++ = dec_int;dec_int>>=8;
      *write_buff_end++ = dec_int;dec_int>>=8;
      *write_buff_end++ = dec_int;dec_int>>=8;
      *write_buff_end++ = dec_int;
    } else {
      qDebug() << "TelescopeTcp(" << name << ")::telescopeGoto: "
               << "communication is too slow, I will ignore this command";
    }
  }
}

void TelescopeTcp::performWriting(void) {
  const int to_write = write_buff_end - write_buff;
  const int rc = send(fd,write_buff,to_write,0);
  if (rc < 0) {
    if (ERRNO != EINTR && ERRNO != EAGAIN) {
      qDebug() << "TelescopeTcp(" << name << ")::performWriting: "
               << "send failed: " << STRERROR(ERRNO);
      hangup();
    }
  } else if (rc > 0) {
    if (rc >= to_write) {
        // everything written
      write_buff_end = write_buff;
    } else {
        // partly written
      memmove(write_buff,write_buff+rc,to_write-rc);
      write_buff_end -= rc;
    }
  }
}

void TelescopeTcp::performReading(void) {
  const int to_read = read_buff + sizeof(read_buff) - read_buff_end;
  const int rc = recv(fd,read_buff_end,to_read,0);
  if (rc < 0) {
    if (ERRNO != EINTR && ERRNO != EAGAIN) {
      qDebug() << "TelescopeTcp(" << name << ")::performReading: "
               << "recv failed: " << STRERROR(ERRNO);
      hangup();
    }
  } else if (rc == 0) {
    qDebug() << "TelescopeTcp(" << name << ")::performReading: "
             << "server has closed the connection";
    hangup();
  } else {
    read_buff_end += rc;
    char *p = read_buff;
    while (read_buff_end-p >= 2) {
      const int size = (int)(                ((unsigned char)(p[0])) |
                              (((unsigned int)(unsigned char)(p[1])) << 8) );
      if (size > (int)sizeof(read_buff) || size < 4) {
        qDebug() << "TelescopeTcp(" << name << ")::performReading: "
                 << "bad packet size: " << size;
        hangup();
        return;
      }
      if (size > read_buff_end-p) {
          // wait for complete packet
        break;
      }
      const int type = (int)(                ((unsigned char)(p[2])) |
                              (((unsigned int)(unsigned char)(p[3])) << 8) );
        // dispatch:
      switch (type) {
        case 0: {
          if (size < 24) {
            qDebug() << "TelescopeTcp(" << name << ")::performReading: "
                     << "type 0: bad packet size: " << size;
            hangup();
            return;
          }
          const long long int server_micros = (long long int)
                 (  ((unsigned long long int)(unsigned char)(p[ 4])) |
                   (((unsigned long long int)(unsigned char)(p[ 5])) <<  8) |
                   (((unsigned long long int)(unsigned char)(p[ 6])) << 16) |
                   (((unsigned long long int)(unsigned char)(p[ 7])) << 24) |
                   (((unsigned long long int)(unsigned char)(p[ 8])) << 32) |
                   (((unsigned long long int)(unsigned char)(p[ 9])) << 40) |
                   (((unsigned long long int)(unsigned char)(p[10])) << 48) |
                   (((unsigned long long int)(unsigned char)(p[11])) << 56) );
          const unsigned int ra_int =
                    ((unsigned int)(unsigned char)(p[12])) |
                   (((unsigned int)(unsigned char)(p[13])) <<  8) |
                   (((unsigned int)(unsigned char)(p[14])) << 16) |
                   (((unsigned int)(unsigned char)(p[15])) << 24);
          const int dec_int =
            (int)(  ((unsigned int)(unsigned char)(p[16])) |
                   (((unsigned int)(unsigned char)(p[17])) <<  8) |
                   (((unsigned int)(unsigned char)(p[18])) << 16) |
                   (((unsigned int)(unsigned char)(p[19])) << 24) );
          const int status =
            (int)(  ((unsigned int)(unsigned char)(p[20])) |
                   (((unsigned int)(unsigned char)(p[21])) <<  8) |
                   (((unsigned int)(unsigned char)(p[22])) << 16) |
                   (((unsigned int)(unsigned char)(p[23])) << 24) );

          position_pointer++;
          if (position_pointer >= end_position) position_pointer = positions;
          position_pointer->server_micros = server_micros;
          position_pointer->client_micros = GetNow();
          const double ra  =  ra_int * (M_PI/(unsigned int)0x80000000);
          const double dec = dec_int * (M_PI/(unsigned int)0x80000000);
          const double cdec = cos(dec);
          position_pointer->pos[0] = cos(ra)*cdec;
          position_pointer->pos[1] = sin(ra)*cdec;
          position_pointer->pos[2] = sin(dec);
          position_pointer->status = status;
//          qDebug() << "TelescopeTcp(" << name << ")::performReading: "
////                  "Server Time: " << server_micros
//               << PrintRaDec(ra_int,dec_int)
//              ;
        } break;
        default:
          qDebug() << "TelescopeTcp(" << name << ")::performReading: "
                   << "ignoring unknown packet, type: " << type;
          break;
      }
      p += size;
    }
    if (p >= read_buff_end) {
        // everything handled
      read_buff_end = read_buff;
    } else {
        // partly handled
      memmove(read_buff,p,read_buff_end-p);
      read_buff_end -= (p-read_buff);
    }
  }
}

Vec3d TelescopeTcp::getObsJ2000Pos(const Navigator*) const {
  if (position_pointer->client_micros == 0x7FFFFFFFFFFFFFFFLL) {
    return Vec3d(0,0,0);
  }
  const long long int now = GetNow() - time_delay;
  const Position *p = position_pointer;
  do {
    const Position *pp = p;
    if (pp == positions) pp = end_position;
    pp--;
    if (pp->client_micros == 0x7FFFFFFFFFFFFFFFLL) break;
    if (pp->client_micros <= now && now <= p->client_micros) {
      if (pp->client_micros != p->client_micros) {
        Vec3d rval = p->pos * (now - pp->client_micros)
                   + pp->pos * (p->client_micros - now);
        double f = rval.lengthSquared();
        if (f > 0.0) {
          return (1.0/sqrt(f))*rval;
        }
      }
      break;
    }
    p = pp;
  } while (p != position_pointer);
  return p->pos;
}

void TelescopeTcp::prepareSelectFds(fd_set &read_fds,fd_set &write_fds,
                                    int &fd_max) {
  if (IS_INVALID_SOCKET(fd)) {
      // try reconnecting
    const long long int now = GetNow();
    if (now < end_of_timeout) return;
    end_of_timeout = now + 5000000;
    fd = socket(AF_INET,SOCK_STREAM,0);
    if (IS_INVALID_SOCKET(fd)) {
      qDebug() << "TelescopeTcp(" << name << ")::prepareSelectFds: "
               << "socket() failed: " << STRERROR(ERRNO);
      return;
    }
    if (SET_NONBLOCKING_MODE(fd) != 0) {
      qDebug() << "TelescopeTcp(" << name << ")::prepareSelectFds: "
               << "could not set nonblocking mode: " << STRERROR(ERRNO);
      hangup();
      return;
    }
    if (connect(fd,(struct sockaddr*)(&address),sizeof(address)) != 0) {
      if (ERRNO != EINPROGRESS && ERRNO != EAGAIN) {
        qDebug() << "TelescopeTcp(" << name << ")::prepareSelectFds: "
                 << "connect() failed: " << STRERROR(ERRNO);
        hangup();
        return;
      }
      wait_for_connection_establishment = true;
//      qDebug() << "TelescopeTcp(" << name << ")::prepareSelectFds: "
//               << "waiting for connection establishment";
    } else {
      wait_for_connection_establishment = false;
      qDebug() << "TelescopeTcp(" << name << ")::prepareSelectFds: "
               << "connection established";
        // connection established, wait for next call of prepareSelectFds
    }
  } else {
      // socked is already connected
    if (fd_max < (int)fd) fd_max = (int)fd;
    if (wait_for_connection_establishment) {
      const long long int now = GetNow();
      if (now > end_of_timeout) {
        end_of_timeout = now + 1000000;
        qDebug() << "TelescopeTcp(" << name << ")::prepareSelectFds: "
                 << "connect timeout";
        hangup();
        return;
      }
      FD_SET(fd,&write_fds);
    } else {
      if (write_buff_end > write_buff) FD_SET(fd,&write_fds);
      FD_SET(fd,&read_fds);
    }
  }
}

void TelescopeTcp::handleSelectFds(const fd_set &read_fds,
                                   const fd_set &write_fds) {
  if (!IS_INVALID_SOCKET(fd)) {
    if (wait_for_connection_establishment) {
		if (FD_ISSET(fd,const_cast<fd_set *>(&write_fds))) {
        wait_for_connection_establishment = false;
        int err = 0;
        SOCKLEN_T length = sizeof(err);
        if (getsockopt(fd,SOL_SOCKET,SO_ERROR,(char*)(&err),&length) != 0) {
          qDebug() << "TelescopeTcp(" << name << ")::handleSelectFds: "
                   << "getsockopt failed";
          hangup();
        } else {
          if (err != 0) {
            qDebug() << "TelescopeTcp(" << name << ")::handleSelectFds: "
                     << "connect failed: " << STRERROR(err);
            hangup();
          } else {
            qDebug() << "TelescopeTcp(" << name << ")::handleSelectFds: "
                     << "connection established";
          }
        }
      }
    } else { // connection already established
		if (FD_ISSET(fd,const_cast<fd_set *>(&write_fds))) {
        performWriting();
      }
	  if (!IS_INVALID_SOCKET(fd) && FD_ISSET(fd,const_cast<fd_set *>(&read_fds))) {
        performReading();
      }
    }
  }
}



