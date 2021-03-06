/* Copyright (c) Mark Harmstone 2020
 *
 * This file is part of wscpp.
 *
 * wscpp is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public Licence as published by
 * the Free Software Foundation, either version 3 of the Licence, or
 * (at your option) any later version.
 *
 * wscpp is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public Licence for more details.
 *
 * You should have received a copy of the GNU Lesser General Public Licence
 * along with wscpp.  If not, see <http://www.gnu.org/licenses/>. */

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include "wscpp.h"
#include <sys/types.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <gssapi/gssapi.h>
#endif
#include <string.h>
#include <random>
#include <map>
#include <stdexcept>
#include "wsclient-impl.h"
#include "b64.h"
#include "sha1.h"
#include "gssexcept.h"

using namespace std;

#define MAGIC_STRING "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

namespace ws {
	client::client(const string& host, uint16_t port, const string& path,
		       const client_msg_handler& msg_handler, const client_disconn_handler& disconn_handler) {
		impl = new client_pimpl(*this, host, port, path, msg_handler, disconn_handler);
	}

	void client_pimpl::open_connexion() {
		int wsa_error = 0;

		struct addrinfo hints, *result;

		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;

		if (getaddrinfo(host.c_str(), to_string(port).c_str(), &hints, &result) != 0)
			throw runtime_error("getaddr failed.");

		try {
			for (struct addrinfo* ai = result; ai; ai = ai->ai_next) {
				char hostname[NI_MAXHOST];

				sock = socket(ai->ai_family, SOCK_STREAM, ai->ai_protocol);
#ifdef _WIN32
				if (sock == INVALID_SOCKET)
					throw runtime_error("socket failed (error " + to_string(WSAGetLastError()) + ")");
#else
				if (sock == -1)
					throw runtime_error("socket failed (error " + to_string(errno) + ")");
#endif

#ifdef _WIN32
				if (connect(sock, ai->ai_addr, (int)ai->ai_addrlen) == SOCKET_ERROR) {
					wsa_error = WSAGetLastError();
					closesocket(sock);
					sock = INVALID_SOCKET;
					continue;
				}
#else
				if (connect(sock, ai->ai_addr, (int)ai->ai_addrlen) == -1) {
					wsa_error = errno;
					close(sock);
					sock = -1;
					continue;
				}
#endif

				// FIXME - only do this if necessary?
				if (getnameinfo(ai->ai_addr, ai->ai_addrlen, hostname, NI_MAXHOST, nullptr, 0, 0) == 0)
					fqdn = hostname;

				break;
			}
		} catch (...) {
			freeaddrinfo(result);
			throw;
		}

		freeaddrinfo(result);

#ifdef _WIN32
		if (sock == INVALID_SOCKET)
#else
		if (sock == -1)
#endif
			throw runtime_error("Could not connect to " + host + " (error " + to_string(wsa_error) + ").");

		open = true;
	}

	client_pimpl::client_pimpl(client& parent, const std::string& host, uint16_t port, const std::string& path,
				   const client_msg_handler& msg_handler, const client_disconn_handler& disconn_handler) :
			parent(parent),
			host(host),
			port(port),
			path(path),
			msg_handler(msg_handler),
			disconn_handler(disconn_handler) {
#ifdef _WIN32
		WSADATA wsa_data;

		if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0)
			throw runtime_error("WSAStartup failed.");
#endif

		try {
			open_connexion();
			send_handshake();

			t = new thread([&]() {
				exception_ptr except;

				try {
					recv_thread();
				} catch (...) {
					except = current_exception();
				}

				open = false;

				if (this->disconn_handler)
					this->disconn_handler(parent, except);
			});
		} catch (...) {
#ifdef _WIN32
			WSACleanup();
#endif
			throw;
		}
	}

	client::~client() {
		delete impl;
	}

	client_pimpl::~client_pimpl() {
#ifdef _WIN32
		if (shutdown(sock, SD_SEND) != SOCKET_ERROR) {
#else
		if (shutdown(sock, SHUT_WR) != -1) {
#endif
			char buf[4096];

			while (::recv(sock, buf, sizeof(buf), 0) > 0) {
			}

#ifdef _WIN32
			closesocket(sock);
#else
			close(sock);
#endif
		}

#ifdef _WIN32
		if (SecIsValidHandle(&cred_handle))
			FreeCredentialsHandle(&cred_handle);

		if (ctx_handle_set)
			DeleteSecurityContext(&ctx_handle);
#endif

		if (t) {
			try {
				t->join();
			} catch (...) {
			}

			delete t;
		}

#ifdef _WIN32
		WSACleanup();
#endif
	}

	string client_pimpl::random_key() {
		mt19937 rng;
		rng.seed(random_device()());
		uniform_int_distribution<mt19937::result_type> dist(0, 0xffffffff);
		uint32_t rand[4];

		for (unsigned int i = 0; i < 4; i++) {
			rand[i] = dist(rng);
		}

		return b64encode(string((char*)rand, 16));
	}

	void client_pimpl::set_send_timeout(unsigned int timeout) const {
#ifdef _WIN32
		DWORD tv = timeout * 1000;

		if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char *)&tv, sizeof(tv)) != 0) {
			int err = WSAGetLastError();

			throw runtime_error("setsockopt returned " + to_string(err) + ".");
		}
#else
		struct timeval tv;
		tv.tv_sec = timeout;
		tv.tv_usec = 0;

		if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char *)&tv, sizeof(tv)) != 0) {
			int err = errno;

			throw runtime_error("setsockopt returned " + to_string(err) + ".");
		}
#endif
	}

	void client_pimpl::send_raw(const string_view& s, unsigned int timeout) const {
		if (timeout != 0)
			set_send_timeout(timeout);

		try {
			auto ret = ::send(sock, s.data(), (int)s.length(), 0);

#ifdef _WIN32
			if (ret == SOCKET_ERROR)
				throw runtime_error("send failed (error " + to_string(WSAGetLastError()) + ")");
#else
			if (ret == -1)
				throw runtime_error("send failed (error " + to_string(errno) + ")");
#endif

			if ((size_t)ret < s.length())
				throw runtime_error("send sent " + to_string(ret) + " bytes, expected " + to_string(s.length()));
		} catch (...) {
			if (timeout != 0)
				set_send_timeout(0);

			throw;
		}

		if (timeout != 0)
			set_send_timeout(0);
	}

	string client_pimpl::recv_http() {
		string buf;

		do {
			char s[4096];
			int bytes = ::recv(sock, s, sizeof(s), MSG_PEEK);

#ifdef _WIN32
			if (bytes == SOCKET_ERROR) {
				auto err = WSAGetLastError();
#else
			if (bytes == -1) {
				auto err = errno;
#endif
				char msg[255];

				sprintf(msg, "recv 1 failed (%u).", err);

				throw runtime_error(msg);
			} else if (bytes == 0) {
				open = false;
				return "";
			}

			buf += string(s, bytes);

			size_t endmsg = string(s, bytes).find("\r\n\r\n");

			if (endmsg != string::npos) {
				int ret;

				ret = ::recv(sock, s, (int)(endmsg + 4), MSG_WAITALL);

#ifdef _WIN32
				if (ret == SOCKET_ERROR)
#else
				if (ret == -1)
#endif
					throw runtime_error("recv 2 failed.");
				else if (ret == 0) {
					open = false;
					return "";
				}

				return buf.substr(0, buf.find("\r\n\r\n") + 4);
			} else {
				int ret = ::recv(sock, s, bytes, MSG_WAITALL);

#ifdef _WIN32
				if (ret == SOCKET_ERROR)
#else
				if (ret == -1)
#endif
					throw runtime_error("recv 4 failed.");
				else if (ret == 0) {
					open = false;
					return "";
				}
			}
		} while (true);
	}

#ifdef _WIN32
static __inline u16string utf8_to_utf16(const string_view& s) {
	u16string ret;

	if (s.empty())
		return u"";

	auto len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.length(), nullptr, 0);

	if (len == 0)
		throw runtime_error("MultiByteToWideChar 1 failed.");

	ret.resize(len);

	len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.length(), (wchar_t*)ret.data(), len);

	if (len == 0)
		throw runtime_error("MultiByteToWideChar 2 failed.");

	return ret;
}

void client_pimpl::send_auth_response(const string_view& auth_type, const string_view& auth_msg, const string& req) {
		SECURITY_STATUS sec_status;
		TimeStamp timestamp;
		char outstr[1024];
		SecBuffer inbufs[2], outbuf;
		SecBufferDesc in, out;
		unsigned long context_attr;
		u16string auth_typew = utf8_to_utf16(auth_type);
		u16string spn;

		if (auth_type == "Negotiate" && fqdn.empty())
			throw runtime_error("Cannot do Negotiate authentication as FQDN not found.");

		if (!SecIsValidHandle(&cred_handle)) {
			sec_status = AcquireCredentialsHandleW(nullptr, (SEC_WCHAR*)auth_typew.c_str(), SECPKG_CRED_OUTBOUND, nullptr,
												   nullptr, nullptr, nullptr, &cred_handle, &timestamp);
			if (FAILED(sec_status)) {
				char s[255];

				sprintf(s, "AcquireCredentialsHandle returned %08lx", sec_status);
				throw runtime_error(s);
			}
		}

		auto auth = b64decode(auth_msg);

		if (!auth_msg.empty()) {
			inbufs[0].cbBuffer = auth.length();
			inbufs[0].BufferType = SECBUFFER_TOKEN;
			inbufs[0].pvBuffer = auth.data();

			inbufs[1].cbBuffer = 0;
			inbufs[1].BufferType = SECBUFFER_EMPTY;
			inbufs[1].pvBuffer = nullptr;

			in.ulVersion = SECBUFFER_VERSION;
			in.cBuffers = 2;
			in.pBuffers = inbufs;
		}

		outbuf.cbBuffer = sizeof(outstr);
		outbuf.BufferType = SECBUFFER_TOKEN;
		outbuf.pvBuffer = outstr;

		out.ulVersion = SECBUFFER_VERSION;
		out.cBuffers = 1;
		out.pBuffers = &outbuf;

		if (auth_type == "Negotiate")
			spn = u"HTTP/" + utf8_to_utf16(fqdn);

		sec_status = InitializeSecurityContextW(&cred_handle, ctx_handle_set ? &ctx_handle : nullptr,
												auth_type == "Negotiate" ? (SEC_WCHAR*)spn.c_str() : nullptr,
												0, 0, SECURITY_NATIVE_DREP, auth_msg.empty() ? nullptr : &in, 0,
												&ctx_handle, &out, &context_attr, &timestamp);
		if (FAILED(sec_status)) {
			char s[255];

			sprintf(s, "InitializeSecurityContext returned %08lx", sec_status);
			throw runtime_error(s);
		}

		ctx_handle_set = true;

		if (sec_status == SEC_I_CONTINUE_NEEDED || sec_status == SEC_I_COMPLETE_AND_CONTINUE ||
			sec_status == SEC_E_OK) {
			auto b64 = b64encode(string_view((char*)outbuf.pvBuffer, outbuf.cbBuffer));

			send_raw(req + "Authorization: " + string(auth_type) + " " + b64 + "\r\n\r\n");

			return;
		}

		// FIXME - SEC_I_COMPLETE_NEEDED (and SEC_I_COMPLETE_AND_CONTINUE)
	}
#else
	void client_pimpl::send_auth_response(const string_view& auth_type, const string_view& auth_msg, const string& req) {
		OM_uint32 major_status, minor_status;
		gss_buffer_desc recv_tok, send_tok, name_buf;
		gss_name_t gss_name;
		string outbuf;

		if (auth_type == "Negotiate" && fqdn.empty())
			throw runtime_error("Cannot do Negotiate authentication as FQDN not found.");

		if (cred_handle != 0) {
			major_status = gss_acquire_cred(&minor_status, GSS_C_NO_NAME/*FIXME?*/, GSS_C_INDEFINITE, GSS_C_NO_OID_SET,
											GSS_C_INITIATE, &cred_handle, nullptr, nullptr);

			if (major_status != GSS_S_COMPLETE)
				throw gss_error("gss_acquire_cred", major_status, minor_status);
		}

		auto auth = b64decode(auth_msg);

		recv_tok.length = auth.length();
		recv_tok.value = auth.data();

		string spn = "HTTP/" + fqdn;

		name_buf.length = spn.length();
		name_buf.value = (void*)spn.data();

		major_status = gss_import_name(&minor_status, &name_buf, GSS_C_NO_OID, &gss_name);
		if (major_status != GSS_S_COMPLETE)
			throw gss_error("gss_import_name", major_status, minor_status);

		major_status = gss_init_sec_context(&minor_status, cred_handle, &ctx_handle, gss_name, GSS_C_NO_OID,
											GSS_C_DELEG_FLAG, GSS_C_INDEFINITE, GSS_C_NO_CHANNEL_BINDINGS,
											&recv_tok, nullptr, &send_tok, nullptr, nullptr);

		if (major_status != GSS_S_CONTINUE_NEEDED && major_status != GSS_S_COMPLETE)
			throw gss_error("gss_init_sec_context", major_status, minor_status);

		if (send_tok.length != 0) {
			outbuf = string((char*)send_tok.value, send_tok.length);

			gss_release_buffer(&minor_status, &send_tok);
		}

		if (!outbuf.empty()) {
			auto b64 = b64encode(outbuf);

			send_raw(req + "Authorization: " + string(auth_type) + " " + b64 + "\r\n\r\n");

			return;
		}
	}
#endif

	void client_pimpl::send_handshake() {
		bool again;
		string key = random_key();
		string req = "GET "s + path + " HTTP/1.1\r\n"
					 "Host: "s + host + ":"s + to_string(port) + "\r\n"
					 "Upgrade: websocket\r\n"
					 "Connection: Upgrade\r\n"
					 "Sec-WebSocket-Key: "s + key + "\r\n"
					 "Sec-WebSocket-Version: 13\r\n";

		send_raw(req + "\r\n"s);

		do {
			string mess = recv_http();

			if (!open)
				throw runtime_error("Socket closed unexpectedly.");

			again = false;

			bool first = true;
			size_t nl = mess.find("\r\n"), nl2 = 0;
			string verb;
			map<string, string> headers;
			unsigned int status = 0;

			do {
				if (first) {
					size_t space = mess.find(" ");

					if (space != string::npos && space <= nl) {
						size_t space2 = mess.find(" ", space + 1);
						string ss;

						if (space2 == string::npos || space2 > nl)
							ss = mess.substr(space + 1, nl - space - 1);
						else
							ss = mess.substr(space + 1, space2 - space - 1);

						try {
							status = stoul(ss);
						} catch (...) {
							throw runtime_error("Error calling stoul on \"" + ss + "\"");
						}
					}

					first = false;
				} else {
					size_t colon = mess.find(": ", nl2);

					if (colon != string::npos)
						headers.emplace(mess.substr(nl2, colon - nl2), mess.substr(colon + 2, nl - colon - 2));
				}

				nl2 = nl + 2;
				nl = mess.find("\r\n", nl2);
			} while (nl != string::npos);

			if (status == 401 && headers.count("WWW-Authenticate") != 0) {
				const auto& h = headers.at("WWW-Authenticate");
				auto st = h.find(" ");
				string_view auth_type, auth_msg;

				if (st == string::npos)
					auth_type = h;
				else {
					auth_type = string_view(h).substr(0, st);
					auth_msg = string_view(h).substr(st + 1);
				}

#ifdef _WIN32
				if (auth_type == "NTLM" || auth_type == "Negotiate")
					send_auth_response(auth_type, auth_msg, req);
#else
				if (auth_type == "Negotiate")
					send_auth_response(auth_type, auth_msg, req);
#endif

				again = true;
				continue;
			}

			if (status != 101)
				throw runtime_error("Server returned HTTP status " + to_string(status) + ", expected 101.");

			if (headers.count("Upgrade") == 0 || headers.count("Connection") == 0 || headers.count("Sec-WebSocket-Accept") == 0 || headers.at("Upgrade") != "websocket" || headers.at("Connection") != "Upgrade")
				throw runtime_error("Malformed response.");

			if (headers.at("Sec-WebSocket-Accept") != b64encode(sha1(key + MAGIC_STRING)))
				throw runtime_error("Invalid value for Sec-WebSocket-Accept.");
		} while (again);
	}

	void client::send(const string_view& payload, enum opcode opcode, unsigned int timeout) const {
		string header;
		uint64_t len = payload.length();

		header.resize(6);
		header[0] = 0x80 | ((uint8_t)opcode & 0xf);

		if (len <= 125) {
			header[1] = 0x80 | (uint8_t)len;
			memset(&header[2], 0, 4);
		} else if (len < 0x10000) {
			header.resize(8);
			header[1] = (uint8_t)0xfe;
			header[2] = (len & 0xff00) >> 8;
			header[3] = len & 0xff;
			memset(&header[4], 0, 4);
		} else {
			header.resize(14);
			header[1] = (uint8_t)0xff;
			header[2] = (uint8_t)((len & 0xff00000000000000) >> 56);
			header[3] = (uint8_t)((len & 0xff000000000000) >> 48);
			header[4] = (uint8_t)((len & 0xff0000000000) >> 40);
			header[5] = (uint8_t)((len & 0xff00000000) >> 32);
			header[6] = (uint8_t)((len & 0xff000000) >> 24);
			header[7] = (uint8_t)((len & 0xff0000) >> 16);
			header[8] = (uint8_t)((len & 0xff00) >> 8);
			header[9] = (uint8_t)(len & 0xff);
			memset(&header[10], 0, 4);
		}

		impl->send_raw(header, timeout);
		impl->send_raw(payload, timeout);
	}

	string client_pimpl::recv(unsigned int len) {
		string s;
		int bytes, err = 0;
		unsigned int left;
		char* buf;

		if (len == 0)
			len = 4096;

		s.resize(len);

		left = len;
		buf = s.data();

		do {
			bytes = ::recv(sock, buf, left, 0);

#ifdef _WIN32
			if (bytes == SOCKET_ERROR) {
				err = WSAGetLastError();
#else
			if (bytes == -1) {
				err = errno;
#endif

#ifdef _WIN32
				if (err == WSAEWOULDBLOCK)
#else
				if (err == EWOULDBLOCK)
#endif
					continue;
				else
					break;
			}

			if (bytes == 0) {
				open = false;
				return "";
			}

			buf += bytes;
			left -= bytes;
		} while (left > 0);

#ifdef _WIN32
		if (bytes == SOCKET_ERROR) {
			if (err == WSAECONNRESET) {
				open = false;
				return "";
			}

			throw runtime_error("recv failed (" + to_string(err) + ").");
		}
#else
		if (bytes == -1) {
			if (err == ECONNRESET) {
				open = false;
				return "";
			}

			throw runtime_error("recv failed (" + to_string(err) + ").");
		}
#endif

		return s;
	}

	void client_pimpl::parse_ws_message(enum opcode opcode, const string& payload) {
		switch (opcode) {
			case opcode::close:
				open = false;
				return;

			case opcode::ping:
				parent.send(payload, opcode::pong);
				break;

			default:
				break;
		}

		if (msg_handler)
			msg_handler(parent, payload, opcode);
	}

	void client_pimpl::recv_thread() {
		while (open) {
			string header = recv(2);

			if (!open)
				break;

			bool fin = (header[0] & 0x80) != 0;
			auto opcode = (enum opcode)(uint8_t)(header[0] & 0xf);
			bool mask = (header[1] & 0x80) != 0;
			uint64_t len = header[1] & 0x7f;

			if (len == 126) {
				string extlen = recv(2);

				if (!open)
					break;

				len = ((uint8_t)extlen[0] << 8) | (uint8_t)extlen[1];
			} else if (len == 127) {
				string extlen = recv(8);

				if (!open)
					break;

				len = (uint8_t)extlen[0];
				len <<= 8;
				len |= (uint8_t)extlen[1];
				len <<= 8;
				len |= (uint8_t)extlen[2];
				len <<= 8;
				len |= (uint8_t)extlen[3];
				len <<= 8;
				len |= (uint8_t)extlen[4];
				len <<= 8;
				len |= (uint8_t)extlen[5];
				len <<= 8;
				len |= (uint8_t)extlen[6];
				len <<= 8;
				len |= (uint8_t)extlen[7];
			}

			string mask_key;
			if (mask) {
				mask_key = recv(4);

				if (!open)
					break;
			}

			string payload = len == 0 ? "" : recv((unsigned int)len);

			if (!open)
				break;

			if (mask) {
				for (unsigned int i = 0; i < payload.length(); i++) {
					payload[i] ^= mask_key[i % 4];
				}
			}

			if (!fin) {
				if (opcode != opcode::invalid)
					last_opcode = opcode;

				payloadbuf += payload;
			} else if (payloadbuf != "") {
				parse_ws_message(last_opcode, payloadbuf + payload);
				payloadbuf = "";
			} else
				parse_ws_message(opcode, payload);
		}
	}

	void client::join() const {
		if (impl->t)
			impl->t->join();
	}

	bool client::is_open() const {
		return impl->open;
	}
}
