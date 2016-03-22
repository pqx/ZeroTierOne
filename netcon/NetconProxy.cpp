/*
 * ZeroTier One - Network Virtualization Everywhere
 * Copyright (C) 2011-2015  ZeroTier, Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * --
 *
 * ZeroTier may be used and distributed under the terms of the GPLv3, which
 * are available at: http://www.gnu.org/licenses/gpl-3.0.html
 *
 * If you would like to embed ZeroTier into a commercial application or
 * redistribute it in a modified binary form, please contact ZeroTier Networks
 * LLC. Start here: http://www.zerotier.com/
 */

#include "NetconEthernetTap.hpp"
#include "../osdep/Phy.hpp"
#include "../node/Utils.hpp"

//#include "common.inc.c"


#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define SOCKS_OPEN			0
#define SOCKS_CONNECT_INIT	1
#define SOCKS_CONNECT_IPV4	2
#define SOCKS_UDP			3 // ?
#define SOCKS_COMPLETE      4

#define CONNECTION_TIMEOUT	8

namespace ZeroTier
{
	void NetconEthernetTap::StartProxy()
	{	
		printf("StartProxy()\n");
		// ref port 1080
		proxyListenPort = 1337;
		struct sockaddr_in in4;
		memset(&in4,0,sizeof(in4));
		in4.sin_family = AF_INET;
		in4.sin_addr.s_addr = Utils::hton((uint32_t)0x00000000); // right now we just listen for TCP @127.0.0.1
		in4.sin_port = Utils::hton((uint16_t)proxyListenPort);
		
		printf("_phy.tcpListen\n");
		proxyListenPhySocket = _phy.tcpListen((const struct sockaddr*)&in4,(void *)this);
		sockstate = SOCKS_OPEN;
    	printf("proxyListenPhySocket = 0x%x\n", proxyListenPhySocket);
	}

	void NetconEthernetTap::phyOnTcpData(PhySocket *sock,void **uptr,void *data,unsigned long len) 
	{
		printf("phyOnTcpData(): 0x%x, len = %d\n", sock, len);
		unsigned char *buf;
		buf = (unsigned char *)data;
        
		// Get connection for this PhySocket
		Connection *conn = getConnection(sock);
		if(!conn) {
			printf("phyOnTcpData(): Unable to locate Connection for sock=0x%x\n", sock);
			return;
		}

		// Write data to lwIP PCB (outgoing)
        if(conn->proxy_conn_state == SOCKS_COMPLETE)
        {
        	if(len) {
	            printf("data = %s, len = %d\n", data, len);
	            memcpy((&conn->txbuf)+(conn->txsz), buf, len);
	   			conn->txsz += len;
	   			handleWrite(conn);
   			}
        }

		if(conn->proxy_conn_state==SOCKS_UDP)
		{
			printf("SOCKS_UDP from client\n");
			// +----+------+------+----------+----------+----------+
			// |RSV | FRAG | ATYP | DST.ADDR | DST.PORT |   DATA   |
			// +----+------+------+----------+----------+----------+
			// | 2  |  1   |  1   | Variable |    2     | Variable |
			// +----+------+------+----------+----------+----------+

			int fragment_num = buf[2];
			int addr_type = buf[3];
		}

		// SOCKS_OPEN
		// +----+----------+----------+
        // |VER | NMETHODS | METHODS  |
        // +----+----------+----------+
        // | 1  |    1     | 1 to 255 |
        // +----+----------+----------+
		if(conn->proxy_conn_state==SOCKS_OPEN)
		{
			if(len >= 3)
			{
				int version = buf[0];
				int methodsLength = buf[1];
				int firstSupportedMethod = buf[2];
				int supportedMethod = 0;

				// Password auth
				if(firstSupportedMethod == 2) {
					supportedMethod = firstSupportedMethod;
				}
				printf(" INFO <ver=%d, meth_len=%d, supp_meth=%d>\n", version, methodsLength, supportedMethod);

				// Send METHOD selection msg
				// +----+--------+
                // |VER | METHOD |
                // +----+--------+
                // | 1  |   1    |
                // +----+--------+
				char reply[2];
				reply[0] = 5; // version
				reply[1] = supportedMethod;
				_phy.streamSend(sock, reply, sizeof(reply));

				// Set state for next message
				conn->proxy_conn_state = SOCKS_CONNECT_INIT;
			}
		}

		// SOCKS_CONNECT
		// +----+-----+-------+------+----------+----------+
        // |VER | CMD |  RSV  | ATYP | DST.ADDR | DST.PORT |
        // +----+-----+-------+------+----------+----------+
        // | 1  |  1  | X'00' |  1   | Variable |    2     |
        // +----+-----+-------+------+----------+----------+
		if(conn->proxy_conn_state==SOCKS_CONNECT_INIT)
		{
			// 4(meta) + 4(ipv4) + 2(port) = 10
			if(len >= 10)
			{
				// Process a SOCKS request
				int version = buf[0];
				int cmd = buf[1];
				int addr_type = buf[3];

				printf("SOCKS REQUEST = <ver=%d, cmd=%d, typ=%d>\n", version, cmd, addr_type);

				// CONNECT request
				if(cmd == 1) {
					// Ipv4
					if(addr_type == 1)
					{
						//printf("IPv4\n");
						int raw_addr;
						struct sockaddr *dst;
						memcpy(&raw_addr, &buf[4], 4);
						char newaddr[16];
						inet_ntop(AF_INET, &raw_addr, (char*)newaddr, INET_ADDRSTRLEN);
						printf("new addr = %s\n", newaddr);

						int rawport, port;
						memcpy(&rawport, &buf[5], 2); 
						port = Utils::ntoh(rawport);
						printf("new port = %d\n", port);

						// Assemble new address
						struct sockaddr_in addr;
						addr.sin_addr.s_addr = IPADDR_ANY;
						addr.sin_family = AF_INET;
						addr.sin_port = Utils::hton(proxyListenPort);

						int fd = socket(AF_INET, SOCK_STREAM, 0);

						if(fd < 0)
							perror("socket");

						int err = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
						if(err < 0)
							perror("connect");
					}

					// Fully-qualified domain name
					if(addr_type == 3)
					{
						// NOTE: Can't separate out port with method used in IPv4 block
						int domain_len = buf[4];
						// Grab Addr:Port
						char raw_addr[domain_len];
						memset(raw_addr, 0, domain_len);
						memcpy(raw_addr, &buf[5], domain_len);

						std::string ip, port, addrstr(raw_addr);
						int del = addrstr.find(":");
						ip = addrstr.substr(0, del);
						port = addrstr.substr(del+1, domain_len);
						
						// Create new lwIP PCB
						PhySocket * new_sock = handleSocketProxy(sock, SOCK_STREAM);
                        
                        printf("new_sock = 0x%x\n", sock);
                        printf("new_sock = 0x%x\n", new_sock);
						if(!new_sock)
							printf("Error while creating proxied-socket\n");

                        // Form address
					    struct sockaddr_in addr; 
					    memset(&addr, '0', sizeof(addr)); 
					    addr.sin_family = AF_INET;
					    addr.sin_port = Utils::hton((uint16_t)atoi(port.c_str()));
//						addr.sin_addr.s_addr = inet_addr(ip.c_str());
                        addr.sin_addr.s_addr = inet_addr("10.5.5.2");

						handleConnectProxy(sock, &addr);

						// Convert connection err code into SOCKS-err-code
						// X'00' succeeded
						// X'01' general SOCKS server failure
						// X'02' connection not allowed by ruleset
						// X'03' Network unreachable
						// X'04' Host unreachable
						// X'05' Connection refused
						// X'06' TTL expired
						// X'07' Command not supported
						// X'08' Address type not supported
						// X'09' to X'FF' unassigned

						// SOCKS_CONNECT_REPLY
						// +----+-----+-------+------+----------+----------+
	        			// |VER | REP |  RSV  | ATYP | BND.ADDR | BND.PORT |
	        			// +----+-----+-------+------+----------+----------+
	        			// | 1  |  1  | X'00' |  1   | Variable |    2     |
	        			// +----+-----+-------+------+----------+----------+

						char reply[len];
						int addr_len = domain_len;
						memset(reply, 0, len); // Create reply buffer at least as big as incoming SOCKS request data
						memcpy(&reply[5],raw_addr,domain_len);
						reply[0] = 5; // version
						reply[1] = 0; // success/err code
						reply[2] = 0; // RSV
						reply[3] = addr_type; // ATYP (1, 3, 4)
						reply[4] = addr_len;
						// reply[5] = 0; // BIND.ADDR
						memcpy(&reply[5+domain_len], &port, 2); // PORT
						_phy.streamSend(sock, reply, sizeof(reply));
                        
                        // Any further data activity on this PhySocket will be considered data to send
                        conn->proxy_conn_state = SOCKS_COMPLETE;
					}
					// CONNECT
				}

				// BIND Request
				if(cmd == 2)
				{
					printf("BIND request\n");
					char raw_addr[15];
					int bind_port;
				}

				// UDP ASSOCIATION Request
				if(cmd == 3)
				{
					// PORT supplied should be port assigned by server in previous msg
					printf("UDP association request\n");

					// SOCKS_CONNECT (Cont.)
					// +----+-----+-------+------+----------+----------+
			        // |VER | CMD |  RSV  | ATYP | DST.ADDR | DST.PORT |
			        // +----+-----+-------+------+----------+----------+
			        // | 1  |  1  | X'00' |  1   | Variable |    2     |
			        // +----+-----+-------+------+----------+----------+

					// NOTE: Similar to cmd==1, should consolidate logic

					int domain_len = buf[4];
					// Grab Addr:Port
					char raw_addr[domain_len];
					memset(raw_addr, 0, domain_len);
					memcpy(raw_addr, &buf[5], domain_len);

					std::string ip, port, addrstr(raw_addr);
					int del = addrstr.find(":");
					ip = addrstr.substr(0, del);
					port = addrstr.substr(del+1, domain_len);

					printf(" addrlen = %d\n", domain_len);
					printf("addrstr = %s\n", addrstr.c_str()); 
					printf("raw_addr = %s\n", raw_addr);
					printf("ip = %s\n", ip.c_str());
					printf("port = %s\n", port.c_str());

					int fd = socket(AF_INET, SOCK_DGRAM, 0);
					if(fd < 0)
						perror("socket");

					struct sockaddr_in addr; 
				    memset(&addr, '0', sizeof(addr)); 
				    addr.sin_family = AF_INET;
				    addr.sin_port = Utils::hton((uint16_t)atoi(port.c_str()));
					addr.sin_addr.s_addr = inet_addr(ip.c_str());

					int err = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
					if(err < 0)
						perror("connect");
					conn->proxy_conn_state == SOCKS_UDP; // FIXME: This needs to be generalized and removed before production
				}

				if(addr_type == 1337)
				{
					// IPv6
				}
			}
		}

		// SOCKS_CONNECT_IPV4
		/*
		if(pconn->proxy_conn_state == SOCKS_CONNECT_IPV4)
		{
			if(len == 4)
			{

			}
		}
		*/

	}


	void NetconEthernetTap::phyOnTcpAccept(PhySocket *sockL,PhySocket *sockN,void **uptrL,void **uptrN,const struct sockaddr *from)
	{
		printf("phyOnTcpAccept(): sockN = 0x%x\n", sockN);
        Connection *newConn = new Connection();
        newConn->sock = sockN;
        _phy.setNotifyWritable(sockN, false);
        _Connections.push_back(newConn);
	}

	void NetconEthernetTap::phyOnTcpConnect(PhySocket *sock,void **uptr,bool success)
	{
		printf("phyOnTcpConnect(): PhySocket = 0x%x\n", sock);
	}

	// Unused -- no UDP or TCP from this thread/Phy<>
	void NetconEthernetTap::phyOnDatagram(PhySocket *sock,void **uptr,const struct sockaddr *from,void *data,unsigned long len)
	{
		printf("phyOnDatagram(): \n");
	}

	void NetconEthernetTap::phyOnTcpClose(PhySocket *sock,void **uptr) 
	{
		printf("phyOnTcpClose(): 0x%x\n", sock);
	}

	void NetconEthernetTap::phyOnTcpWritable(PhySocket *sock,void **uptr, bool lwip_invoked) 
	{
		printf(" phyOnTcpWritable(): sock=0x%x\n", sock);
		processReceivedData(sock,uptr,lwip_invoked);
	}

	// RX data on stream socks and send back over client sock's underlying fd
	void NetconEthernetTap::phyOnFileDescriptorActivity(PhySocket *sock,void **uptr,bool readable,bool writable)
	{
		printf("phyOnFileDescriptorActivity(): 0x%x \n", sock);
		if(readable)
		{
			//ProxyConn *conn = (ProxyConn*)*uptr;
			//if(!conn){
			//	printf("\t!conn");
			//	return;
			//}
			char buf[50];
			memset(buf, 0, sizeof(buf));

			printf("Activity(R)->socket() = %d\n", _phy.getDescriptor(sock));
			//printf("Activity(W)->socket() = %d\n", conn->fd);
			int n_read = read(_phy.getDescriptor(sock), buf, sizeof(buf));
			//printf("  read = %d\n", n_read);
			//int n_sent = write(conn->fd, buf, n_read);
			//printf("buf = %s\n", buf);
			//printf("  sent = %d\n", n_sent);
		}
		if(writable)
		{
			printf(" writable\n");
		}
	}
}