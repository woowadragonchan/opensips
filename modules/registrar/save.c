/*
 * $Id$
 *
 * Process REGISTER request and send reply
 *
 * Copyright (C) 2001-2003 FhG Fokus
 *
 * This file is part of openser, a free SIP server.
 *
 * openser is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * openser is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with this program; if not, write to the Free Software 
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * History:
 * ----------
 * 2003-01-27 next baby-step to removing ZT - PRESERVE_ZT (jiri)
 * 2003-02-28 scrathcpad compatibility abandoned (jiri)
 * 2003-03-21 save_noreply added, patch provided by Maxim Sobolev 
 *            <sobomax@portaone.com> (janakj)
 * 2005-07-11  added sip_natping_flag for nat pinging with SIP method
 *             instead of UDP package (bogdan)
 */


#include "../../str.h"
#include "../../parser/parse_to.h"
#include "../../dprint.h"
#include "../../trim.h"
#include "../../ut.h"
#include "../usrloc/usrloc.h"
#include "../../qvalue.h"
#include "common.h"
#include "sip_msg.h"
#include "rerrno.h"
#include "reply.h"
#include "reg_mod.h"
#include "regtime.h"
#include "save.h"

static int mem_only = 0;

void remove_cont(urecord_t* _r, ucontact_t* _c)
{
	if (_c->prev) {
		_c->prev->next = _c->next;
		if (_c->next) {
			_c->next->prev = _c->prev;
		}
	} else {
		_r->contacts = _c->next;
		if (_c->next) {
			_c->next->prev = 0;
		}
	}
}


void move_on_top(urecord_t* _r, ucontact_t* _c)
{
	ucontact_t* prev;

	if (!_r->contacts) return;
	if (_c->prev == 0) return;

	prev = _c->prev;

	remove_cont(_r, _c);
	
	_c->next = _r->contacts;
	_c->prev = 0;

	_r->contacts->prev = _c;
	_r->contacts = _c;
}


/*
 * Process request that contained a star, in that case, 
 * we will remove all bindings with the given username 
 * from the usrloc and return 200 OK response
 */
static inline int star(udomain_t* _d, str* _a)
{
	urecord_t* r;
	ucontact_t* c;
	
	ul.lock_udomain(_d);

	if (!ul.get_urecord(_d, _a, &r)) {
		c = r->contacts;
		while(c) {
			if (mem_only) {
				c->flags |= FL_MEM;
			} else {
				c->flags &= ~FL_MEM;
			}
			c = c->next;
		}
	}

	if (ul.delete_urecord(_d, _a, 0) < 0) {
		LOG(L_ERR, "star(): Error while removing record from usrloc\n");
		
		     /* Delete failed, try to get corresponding
		      * record structure and send back all existing
		      * contacts
		      */
		rerrno = R_UL_DEL_R;
		if (!ul.get_urecord(_d, _a, &r)) {
			build_contact(r->contacts);
		}
		ul.unlock_udomain(_d);
		return -1;
	}
	ul.unlock_udomain(_d);
	return 0;
}


#include "../../socket_info.h"
/*
 */
static struct socket_info *get_sock_hdr(struct sip_msg *msg)
{
	struct socket_info *sock;
	struct hdr_field *hf;
	unsigned int port_no;
	str sock_str;
	str port;
	char *p;

	if (parse_headers( msg, HDR_EOH_F, 0) == -1) {
		LOG(L_ERR,"ERROR:registrar:get_sock_hdr: failed to parse message\n");
		return 0;
	}

	for (hf=msg->headers; hf; hf=hf->next) {
		if (hf->name.len==sock_hdr_name.len &&
		strncasecmp(hf->name.s, sock_hdr_name.s, sock_hdr_name.len)==0 )
			break;
	}

	/* hdr found? */
	if (hf==0)
		return 0;

	trim_len( sock_str.len, sock_str.s, hf->body );
	if (sock_str.len==0)
		return 0;

	for(p=sock_str.s+(sock_str.len-1) ; p>sock_str.s && *p!='_' ; p-- );

	if (p!=sock_str.s) {
		/* has port */
		port.s = p+1;
		port.len = (sock_str.s + sock_str.len) - port.s;
		if (str2int( &port, &port_no)!=0) {
			LOG(L_ERR,"ERROR:registrar:get_sock_hdr: bad port <%.*s> in "
				"socket\n",port.len,port.s);
			port_no = SIP_PORT;
		}
		sock_str.len = (port.s - sock_str.s) - 1;
	} else {
		/* no port */
		port_no = SIP_PORT;
	}

	sock = grep_sock_info( &sock_str, (unsigned short)port_no,
			PROTO_NONE /*FIXME - correct proto?*/);

	DBG("DEBUG:registrar:get_sock_hdr: <%.*s>:%d -> p=%p\n",
		sock_str.len,sock_str.s,port_no,sock );

	return sock;
}



/*
 * Process request that contained no contact header
 * field, it means that we have to send back a response
 * containing a list of all existing bindings for the
 * given username (in To HF)
 */
static inline int no_contacts(udomain_t* _d, str* _a)
{
	urecord_t* r;
	int res;
	
	ul.lock_udomain(_d);
	res = ul.get_urecord(_d, _a, &r);
	if (res < 0) {
		rerrno = R_UL_GET_R;
		LOG(L_ERR, "no_contacts(): Error while retrieving record from usrloc\n");
		ul.unlock_udomain(_d);
		return -1;
	}
	
	if (res == 0) {  /* Contacts found */
		build_contact(r->contacts);
	}
	ul.unlock_udomain(_d);
	return 0;
}


/*
 *
 */
static inline ucontact_info_t* pack_contact(struct sip_msg* _m, contact_t* _c,
			int _e, int _f1, int _f2)
{
	static ucontact_info_t ci;
	static str callid;
	int_str rcv_avp;
	int_str val;

	rcv_avp.n=rcv_avp_no;
	memset( &ci, 0, sizeof(ucontact_info_t));

	/* Calculate q value of the contact */
	if (calc_contact_q(_c->q, &ci.q) < 0) {
		LOG(L_ERR, "ERROR:usrloc:pack_contact: failed to calculate q\n");
		goto error;
	}

	/* Get callid of the message */
	callid = _m->callid->body;
	trim_trailing(&callid);
	ci.callid = &callid;

	/* Get CSeq number of the message */
	if (str2int(&get_cseq(_m)->number, (unsigned int*)&ci.cseq) < 0) {
		rerrno = R_INV_CSEQ;
		LOG(L_ERR, "ERROR:usrloc:pack_contact: failed to convert "
			"cseq number\n");
		goto error;
	}

	/* set received URI */
	if (_c->received) {
		ci.received = &_c->received->body;
	} else if (search_first_avp(0, rcv_avp, &val)) {
		ci.received = val.s;
	} else {
		ci.received = 0;
	}

	/* set received socket */
	if (sock_flag!=-1 && (_m->flags&sock_flag)!=0) {
		ci.sock = get_sock_hdr(_m);
		if (ci.sock==0)
			ci.sock = _m->rcv.bind_address;
	} else {
		ci.sock = _m->rcv.bind_address;
	}

	/* set expire time */
	ci.expires = _e;

	/* set flags */
	ci.flags1 = _f1;
	ci.flags2 = _f2;

	return &ci;
error:
	return 0;
}


/*
 * Message contained some contacts, but record with same address
 * of record was not found so we have to create a new record
 * and insert all contacts from the message that have expires
 * > 0
 */
static inline int insert_contacts(struct sip_msg* _m, contact_t* _c,
											udomain_t* _d, str* _a, str *_ua)
{
	ucontact_info_t* ci;
	urecord_t* r;
	ucontact_t* c;
	unsigned int flags;
	int num;
	int e;

	/* is nated flag */
	if (nat_flag!=-1 && _m->flags&nat_flag)
		flags = FL_NAT;
	else
		flags = FL_NONE;
	/* nat type flag */
	if (sip_natping_flag!=-1 && _m->flags&sip_natping_flag)
		flags |= FL_NAT_SIPPING;

	flags |= mem_only;

	for( num=0,r=0 ; _c ; _c = get_next_contact(_c) ) {
		if (calc_contact_expires(_m, _c->expires, &e) < 0) {
			LOG(L_ERR,"ERROR:usrloc:insert_contacts: failed to "
				"calculate expires\n");
			goto error;
		}
		/* Skip contacts with zero expires */
		if (e == 0)
			continue;

		if (max_contacts && (num >= max_contacts)) {
			rerrno = R_TOO_MANY;
			goto error;
		}
		num++;

		if (r==0) {
			if (ul.insert_urecord(_d, _a, &r) < 0) {
				rerrno = R_UL_NEW_R;
				LOG(L_ERR, "ERROR:usrloc:insert_contacts: failed to insert "
					"new record structure\n");
				goto error;
			}
		}

		/* pack a contact_info */
		if ( (ci=pack_contact( _m, _c, e, flags, 0))==0 )
			goto error;

		/* add user-agent */
		ci->user_agent = _ua;

		if (ul.insert_ucontact( r, &_c->uri, ci, &c) < 0) {
			rerrno = R_UL_INS_C;
			LOG(L_ERR, "ERROR:usrloc:insert_contacts: failed to insert "
				"contact\n");
			goto error;
		}
	}

	if (r) {
		if (!r->contacts) {
			ul.delete_urecord( 0, 0, r);
		} else {
			build_contact(r->contacts);
		}
	}

	return 0;
error:
	if (r)
		ul.delete_urecord(_d, _a, r);
	return -1;
}


static int test_max_contacts(struct sip_msg* _m, urecord_t* _r, contact_t* _c)
{
	int num;
	int e;
	ucontact_t* ptr, *cont;
	
	num = 0;
	ptr = _r->contacts;
	while(ptr) {
		if (VALID_CONTACT(ptr, act_time)) {
			num++;
		}
		ptr = ptr->next;
	}
	DBG("test_max_contacts: %d valid contacts\n", num);
	
	while(_c) {
		if (calc_contact_expires(_m, _c->expires, &e) < 0) {
			LOG(L_ERR, "test_max_contacts: Error while calculating expires\n");
			return -1;
		}
		
		if (ul.get_ucontact(_r, &_c->uri, &cont) > 0) {
			     /* Contact not found */
			if (e != 0) num++;
		} else {
			if (e == 0) num--;
		}
		
		_c = get_next_contact(_c);
	}
	
	DBG("test_max_contacts: %d contacts after commit\n", num);
	if (num > max_contacts) {
		rerrno = R_TOO_MANY;
		return 1;
	}
	
	return 0;
}


/*
 * Message contained some contacts and appropriate
 * record was found, so we have to walk through
 * all contacts and do the following:
 * 1) If contact in usrloc doesn't exists and
 *    expires > 0, insert new contact
 * 2) If contact in usrloc exists and expires
 *    > 0, update the contact
 * 3) If contact in usrloc exists and expires
 *    == 0, delete contact
 */
static inline int update(struct sip_msg* _m, urecord_t* _r, contact_t* _c,
																	str* _ua)
{
	ucontact_info_t *ci;
	ucontact_t* c;
	int e;
	int set, reset;
	unsigned int flags;
	int_str rcv_avp;

	rcv_avp.n=rcv_avp_no;
	/* is nated flag */
	if (nat_flag!=-1 && _m->flags&nat_flag)
		flags = FL_NAT;
	else
		flags = FL_NONE;
	/* nat type flag */
	if (sip_natping_flag!=-1 && _m->flags&sip_natping_flag)
		flags |= FL_NAT_SIPPING;

	if (max_contacts) {
		if (test_max_contacts(_m, _r, _c) != 0 )
			goto error;
	}

	_c = get_first_contact(_m);

	while(_c) {
		if (calc_contact_expires(_m, _c->expires, &e) < 0) {
			LOG(L_ERR, "update(): Error while calculating expires\n");
			goto error;
		}

		if (ul.get_ucontact(_r, &_c->uri, &c) > 0) {
			/* Contact not found -> expired? */
			if (e==0)
				continue;

			/* pack a contact_info */
			if ( (ci=pack_contact( _m, _c, e, flags, 0))==0 )
			goto error;

			/* add user-agent */
			ci->user_agent = _ua;

			if (ul.insert_ucontact( _r, &_c->uri, ci, &c) < 0) {
				rerrno = R_UL_INS_C;
				LOG(L_ERR, "ERROR:usrloc:update: failed to insert "
					"contact\n");
				goto error;
			}
		} else {
			/* Contact not found */
			if (e == 0) {
				/* it's expired */
				if (mem_only) {
					c->flags |= FL_MEM;
				} else {
					c->flags &= ~FL_MEM;
				}

				if (ul.delete_ucontact(_r, c) < 0) {
					rerrno = R_UL_DEL_C;
					LOG(L_ERR, "update(): Error while deleting contact\n");
					return -5;
				}
			} else {
				/* do updated */
				set = flags | mem_only;
				reset = ~(flags | mem_only) & (FL_NAT|FL_MEM|FL_NAT_SIPPING);

				/* pack a contact_info */
				if ( (ci=pack_contact( _m, _c, e, set, reset))==0 )
					goto error;

				/* add user-agent */
				ci->user_agent = _ua;

				if (ul.update_ucontact(c, ci) < 0) {
					rerrno = R_UL_UPD_C;
					LOG(L_ERR, "update(): Error while updating contact\n");
					return -8;
				}

				if (desc_time_order) {
					move_on_top(_r, c);
				}
			}
		}
		_c = get_next_contact(_c);
	}

	return 0;
error:
	return -1;
}


/* 
 * This function will process request that
 * contained some contact header fields
 */
static inline int add_contacts(struct sip_msg* _m, contact_t* _c,
													udomain_t* _d, str* _a)
{
	str ua = {"n/a",3};
	int res;
	urecord_t* r;

	ua.s = 0;
	ua.len = 0;
	if (parse_headers(_m, HDR_USERAGENT_F, 0) != -1 && _m->user_agent &&
	_m->user_agent->body.len > 0) {
		ua.len = _m->user_agent->body.len;
		ua.s = _m->user_agent->body.s;
	}

	ul.lock_udomain(_d);
	res = ul.get_urecord(_d, _a, &r);
	if (res < 0) {
		rerrno = R_UL_GET_R;
		LOG(L_ERR, "contacts(): Error while retrieving record from usrloc\n");
		ul.unlock_udomain(_d);
		return -2;
	}

	if (res == 0) { /* Contacts found */
		if (update(_m, r, _c, &ua) < 0) {
			LOG(L_ERR, "contacts(): Error while updating record\n");
			build_contact(r->contacts);
			ul.release_urecord(r);
			ul.unlock_udomain(_d);
			return -3;
		}
		build_contact(r->contacts);
		ul.release_urecord(r);
	} else {
		if (insert_contacts(_m, _c, _d, _a, &ua) < 0) {
			LOG(L_ERR, "contacts(): Error while inserting record\n");
			ul.unlock_udomain(_d);
			return -4;
		}
	}
	ul.unlock_udomain(_d);
	return 0;
}


/*
 * Process REGISTER request and save it's contacts
 */
static inline int save_real(struct sip_msg* _m, udomain_t* _t, char* _s, int doreply)
{
	contact_t* c;
	int st;
	str aor;

	rerrno = R_FINE;

	if (parse_message(_m) < 0) {
		goto error;
	}

	if (check_contacts(_m, &st) > 0) {
		goto error;
	}
	
	get_act_time();
	c = get_first_contact(_m);

	if (extract_aor(&get_to(_m)->uri, &aor) < 0) {
		LOG(L_ERR, "save(): Error while extracting Address Of Record\n");
		goto error;
	}

	if (c == 0) {
		if (st) {
			if (star(_t, &aor) < 0) goto error;
		} else {
			if (no_contacts(_t, &aor) < 0) goto error;
		}
	} else {
		if (add_contacts(_m, c, _t, &aor) < 0) goto error;
	}

	if (doreply && (send_reply(_m) < 0)) return -1;
	else return 1;
	
 error:
	if (doreply) send_reply(_m);
	return 0;
}


/*
 * Process REGISTER request and save it's contacts
 */
int save(struct sip_msg* _m, char* _t, char* _s)
{
	mem_only = FL_NONE;
	return save_real(_m, (udomain_t*)_t, _s, 1);
}


/*
 * Process REGISTER request and save it's contacts, do not send any replies
 */
int save_noreply(struct sip_msg* _m, char* _t, char* _s)
{
	mem_only = FL_NONE;
	return save_real(_m, (udomain_t*)_t, _s, 0);
}


/*
 * Update memory cache only
 */
int save_memory(struct sip_msg* _m, char* _t, char* _s)
{
	mem_only = FL_MEM;
	return save_real(_m, (udomain_t*)_t, _s, 1);
}
