/*
    protocol_key.c -- handle the meta-protocol, key exchange
    Copyright (C) 1999-2005 Ivo Timmermans,
                  2000-2012 Guus Sliepen <guus@tinc-vpn.org>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "system.h"

#include "splay_tree.h"
#include "cipher.h"
#include "connection.h"
#include "crypto.h"
#include "ecdh.h"
#include "logger.h"
#include "net.h"
#include "netutl.h"
#include "node.h"
#include "prf.h"
#include "protocol.h"
#include "utils.h"
#include "xalloc.h"

static bool mykeyused = false;

void send_key_changed(void) {
	splay_node_t *node;
	connection_t *c;

	send_request(everyone, "%d %x %s", KEY_CHANGED, rand(), myself->name);

	/* Immediately send new keys to directly connected nodes to keep UDP mappings alive */

	for(node = connection_tree->head; node; node = node->next) {
		c = node->data;
		if(c->status.active && c->node && c->node->status.reachable)
			send_ans_key(c->node);
	}
}

bool key_changed_h(connection_t *c, const char *request) {
	char name[MAX_STRING_SIZE];
	node_t *n;

	if(sscanf(request, "%*d %*x " MAX_STRING, name) != 1) {
		logger(DEBUG_ALWAYS, LOG_ERR, "Got bad %s from %s (%s)", "KEY_CHANGED",
			   c->name, c->hostname);
		return false;
	}

	if(seen_request(request))
		return true;

	n = lookup_node(name);

	if(!n) {
		logger(DEBUG_ALWAYS, LOG_ERR, "Got %s from %s (%s) origin %s which does not exist",
			   "KEY_CHANGED", c->name, c->hostname, name);
		return true;
	}

	n->status.validkey = false;
	n->last_req_key = 0;

	/* Tell the others */

	if(!tunnelserver)
		forward_request(c, request);

	return true;
}

bool send_req_key(node_t *to) {
	if(experimental && OPTION_VERSION(to->options) >= 2) {
		if(!node_read_ecdsa_public_key(to))
			send_request(to->nexthop->connection, "%d %s %s %d", REQ_KEY, myself->name, to->name, REQ_PUBKEY);
	}
	return send_request(to->nexthop->connection, "%d %s %s", REQ_KEY, myself->name, to->name);
}

/* REQ_KEY is overloaded to allow arbitrary requests to be routed between two nodes. */

static bool req_key_ext_h(connection_t *c, const char *request, node_t *from, int reqno) {
	switch(reqno) {
		case REQ_PUBKEY: {
			char *pubkey = ecdsa_get_base64_public_key(&myself->connection->ecdsa);
			send_request(from->nexthop->connection, "%d %s %s %d %s", REQ_KEY, myself->name, from->name, ANS_PUBKEY, pubkey);
			free(pubkey);
			return true;
		}

		case ANS_PUBKEY: {
			if(node_read_ecdsa_public_key(from)) {
				logger(DEBUG_ALWAYS, LOG_WARNING, "Got ANS_PUBKEY from %s (%s) even though we already have his pubkey", from->name, from->hostname);
				return true;
			}

			char pubkey[4096];
			if(sscanf(request, "%*d %*s %*s %*d " MAX_STRING, pubkey) != 1 || !ecdsa_set_base64_public_key(&from->ecdsa, pubkey)) {
				logger(DEBUG_ALWAYS, LOG_ERR, "Got bad %s from %s (%s): %s", "ANS_PUBKEY", from->name, from->hostname, "invalid pubkey");
				return true;
			}

			logger(DEBUG_ALWAYS, LOG_INFO, "Learned ECDSA public key from %s (%s)", from->name, from->hostname);
			append_config_file(from->name, "ECDSAPublicKey", pubkey);
			return true;
		}

		default:
			logger(DEBUG_ALWAYS, LOG_ERR, "Unknown extended REQ_KEY request from %s (%s): %s", from->name, from->hostname, request);
			return true;
	}
}

bool req_key_h(connection_t *c, const char *request) {
	char from_name[MAX_STRING_SIZE];
	char to_name[MAX_STRING_SIZE];
	node_t *from, *to;
	int reqno = 0;

	if(sscanf(request, "%*d " MAX_STRING " " MAX_STRING " %d", from_name, to_name, &reqno) < 2) {
		logger(DEBUG_ALWAYS, LOG_ERR, "Got bad %s from %s (%s)", "REQ_KEY", c->name,
			   c->hostname);
		return false;
	}

	if(!check_id(from_name) || !check_id(to_name)) {
		logger(DEBUG_ALWAYS, LOG_ERR, "Got bad %s from %s (%s): %s", "REQ_KEY", c->name, c->hostname, "invalid name");
		return false;
	}

	from = lookup_node(from_name);

	if(!from) {
		logger(DEBUG_ALWAYS, LOG_ERR, "Got %s from %s (%s) origin %s which does not exist in our connection list",
			   "REQ_KEY", c->name, c->hostname, from_name);
		return true;
	}

	to = lookup_node(to_name);

	if(!to) {
		logger(DEBUG_ALWAYS, LOG_ERR, "Got %s from %s (%s) destination %s which does not exist in our connection list",
			   "REQ_KEY", c->name, c->hostname, to_name);
		return true;
	}

	/* Check if this key request is for us */

	if(to == myself) {			/* Yes, send our own key back */
		if(experimental && reqno)
			return req_key_ext_h(c, request, from, reqno);

		send_ans_key(from);
	} else {
		if(tunnelserver)
			return true;

		if(!to->status.reachable) {
			logger(DEBUG_ALWAYS, LOG_WARNING, "Got %s from %s (%s) destination %s which is not reachable",
				"REQ_KEY", c->name, c->hostname, to_name);
			return true;
		}

		send_request(to->nexthop->connection, "%s", request);
	}

	return true;
}

bool send_ans_key_ecdh(node_t *to) {
	int siglen = ecdsa_size(&myself->connection->ecdsa);
	char key[(ECDH_SIZE + siglen) * 2 + 1];

	if(!ecdh_generate_public(&to->ecdh, key))
		return false;

	if(!ecdsa_sign(&myself->connection->ecdsa, key, ECDH_SIZE, key + ECDH_SIZE))
		return false;

	b64encode(key, key, ECDH_SIZE + siglen);

	int result = send_request(to->nexthop->connection, "%d %s %s %s %d %d %zu %d", ANS_KEY,
						myself->name, to->name, key,
						cipher_get_nid(&myself->incipher),
						digest_get_nid(&myself->indigest),
						digest_length(&myself->indigest),
						myself->incompression);

	return result;
}

bool send_ans_key(node_t *to) {
	if(experimental && OPTION_VERSION(to->options) >= 2)
		return send_ans_key_ecdh(to);

	size_t keylen = cipher_keylength(&myself->incipher);
	char key[keylen * 2 + 1];

	cipher_open_by_nid(&to->incipher, cipher_get_nid(&myself->incipher));
	digest_open_by_nid(&to->indigest, digest_get_nid(&myself->indigest), digest_length(&myself->indigest));
	to->incompression = myself->incompression;

	randomize(key, keylen);
	cipher_set_key(&to->incipher, key, false);
	digest_set_key(&to->indigest, key, keylen);

	bin2hex(key, key, keylen);

	// Reset sequence number and late packet window
	mykeyused = true;
	to->received_seqno = 0;
	if(replaywin) memset(to->late, 0, replaywin);

	return send_request(to->nexthop->connection, "%d %s %s %s %d %d %zu %d", ANS_KEY,
						myself->name, to->name, key,
						cipher_get_nid(&to->incipher),
						digest_get_nid(&to->indigest),
						digest_length(&to->indigest),
						to->incompression);
}

bool ans_key_h(connection_t *c, const char *request) {
	char from_name[MAX_STRING_SIZE];
	char to_name[MAX_STRING_SIZE];
	char key[MAX_STRING_SIZE];
        char address[MAX_STRING_SIZE] = "";
        char port[MAX_STRING_SIZE] = "";
	int cipher, digest, maclength, compression, keylen;
	node_t *from, *to;

	if(sscanf(request, "%*d "MAX_STRING" "MAX_STRING" "MAX_STRING" %d %d %d %d "MAX_STRING" "MAX_STRING,
		from_name, to_name, key, &cipher, &digest, &maclength,
		&compression, address, port) < 7) {
		logger(DEBUG_ALWAYS, LOG_ERR, "Got bad %s from %s (%s)", "ANS_KEY", c->name,
			   c->hostname);
		return false;
	}

	if(!check_id(from_name) || !check_id(to_name)) {
		logger(DEBUG_ALWAYS, LOG_ERR, "Got bad %s from %s (%s): %s", "ANS_KEY", c->name, c->hostname, "invalid name");
		return false;
	}

	from = lookup_node(from_name);

	if(!from) {
		logger(DEBUG_ALWAYS, LOG_ERR, "Got %s from %s (%s) origin %s which does not exist in our connection list",
			   "ANS_KEY", c->name, c->hostname, from_name);
		return true;
	}

	to = lookup_node(to_name);

	if(!to) {
		logger(DEBUG_ALWAYS, LOG_ERR, "Got %s from %s (%s) destination %s which does not exist in our connection list",
			   "ANS_KEY", c->name, c->hostname, to_name);
		return true;
	}

	/* Forward it if necessary */

	if(to != myself) {
		if(tunnelserver)
			return true;

		if(!to->status.reachable) {
			logger(DEBUG_ALWAYS, LOG_WARNING, "Got %s from %s (%s) destination %s which is not reachable",
				   "ANS_KEY", c->name, c->hostname, to_name);
			return true;
		}

		if(!*address && from->address.sa.sa_family != AF_UNSPEC) {
			char *address, *port;
			logger(DEBUG_PROTOCOL, LOG_DEBUG, "Appending reflexive UDP address to ANS_KEY from %s to %s", from->name, to->name);
			sockaddr2str(&from->address, &address, &port);
			send_request(to->nexthop->connection, "%s %s %s", request, address, port);
			free(address);
			free(port);
			return true;
		}

		return send_request(to->nexthop->connection, "%s", request);
	}

	/* Check and lookup cipher and digest algorithms */

	if(!cipher_open_by_nid(&from->outcipher, cipher)) {
		logger(DEBUG_ALWAYS, LOG_ERR, "Node %s (%s) uses unknown cipher!", from->name, from->hostname);
		return false;
	}

	if(!digest_open_by_nid(&from->outdigest, digest, maclength)) {
		logger(DEBUG_ALWAYS, LOG_ERR, "Node %s (%s) uses unknown digest!", from->name, from->hostname);
		return false;
	}

	if(maclength != digest_length(&from->outdigest)) {
		logger(DEBUG_ALWAYS, LOG_ERR, "Node %s (%s) uses bogus MAC length!", from->name, from->hostname);
		return false;
	}

	if(compression < 0 || compression > 11) {
		logger(DEBUG_ALWAYS, LOG_ERR, "Node %s (%s) uses bogus compression level!", from->name, from->hostname);
		return true;
	}

	from->outcompression = compression;

	/* ECDH or old-style key exchange? */
	
	if(experimental && OPTION_VERSION(from->options) >= 2) {
		/* Check if we already have an ECDSA public key for this node. */

		if(!node_read_ecdsa_public_key(from)) {
			logger(DEBUG_ALWAYS, LOG_ERR, "No ECDSA public key known for %s (%s), cannot verify ECDH key exchange!", from->name, from->hostname);
			return true;
		}

		int siglen = ecdsa_size(&from->ecdsa);
		int keylen = b64decode(key, key, sizeof key);

		if(keylen != ECDH_SIZE + siglen) {
			logger(DEBUG_ALWAYS, LOG_ERR, "Node %s (%s) uses wrong keylength! %d != %d", from->name, from->hostname, keylen, ECDH_SIZE + siglen);
			return true;
		}

		if(ECDH_SHARED_SIZE < cipher_keylength(&from->outcipher)) {
			logger(DEBUG_ALWAYS, LOG_ERR, "ECDH key too short for cipher of %s!", from->name);
			return true;
		}

		if(!ecdsa_verify(&from->ecdsa, key, ECDH_SIZE, key + ECDH_SIZE)) {
			logger(DEBUG_ALWAYS, LOG_ERR, "Possible intruder %s (%s): %s", from->name, from->hostname, "invalid ECDSA signature");
			return true;
		}

		if(!from->ecdh) {
			if(!send_ans_key_ecdh(from))
				return true;
		}

		char shared[ECDH_SHARED_SIZE * 2 + 1];

		if(!ecdh_compute_shared(&from->ecdh, key, shared))
			return true;

		/* Update our crypto end */

		size_t mykeylen = cipher_keylength(&myself->incipher);
		size_t hiskeylen = cipher_keylength(&from->outcipher);

		char *mykey;
		char *hiskey;
		char *seed;
		
		if(strcmp(myself->name, from->name) < 0) {
			mykey = key;
			hiskey = key + mykeylen * 2;
			xasprintf(&seed, "tinc UDP key expansion %s %s", myself->name, from->name);
		} else {
			mykey = key + hiskeylen * 2;
			hiskey = key;
			xasprintf(&seed, "tinc UDP key expansion %s %s", from->name, myself->name);
		}

		if(!prf(shared, ECDH_SHARED_SIZE, seed, strlen(seed), key, hiskeylen * 2 + mykeylen * 2))
			return true;

		free(seed);

		cipher_open_by_nid(&from->incipher, cipher_get_nid(&myself->incipher));
		digest_open_by_nid(&from->indigest, digest_get_nid(&myself->indigest), digest_length(&myself->indigest));
		from->incompression = myself->incompression;

		cipher_set_key(&from->incipher, mykey, false);
		digest_set_key(&from->indigest, mykey + mykeylen, mykeylen);

		cipher_set_key(&from->outcipher, hiskey, true);
		digest_set_key(&from->outdigest, hiskey + hiskeylen, hiskeylen);

		// Reset sequence number and late packet window
		mykeyused = true;
		from->received_seqno = 0;
		if(replaywin)
			memset(from->late, 0, replaywin);

		if(strcmp(myself->name, from->name) < 0)
			memmove(key, key + mykeylen * 2, hiskeylen * 2);
	} else {
		keylen = hex2bin(key, key, sizeof key);

		if(keylen != cipher_keylength(&from->outcipher)) {
			logger(DEBUG_ALWAYS, LOG_ERR, "Node %s (%s) uses wrong keylength!", from->name, from->hostname);
			return true;
		}

		/* Update our copy of the origin's packet key */

		cipher_set_key(&from->outcipher, key, true);
		digest_set_key(&from->outdigest, key, keylen);
	}

	from->status.validkey = true;
	from->sent_seqno = 0;

	if(*address && *port) {
		logger(DEBUG_PROTOCOL, LOG_DEBUG, "Using reflexive UDP address from %s: %s port %s", from->name, address, port);
		sockaddr_t sa = str2sockaddr(address, port);
		update_node_udp(from, &sa);
	}

	if(from->options & OPTION_PMTU_DISCOVERY)
		send_mtu_probe(from);

	return true;
}
