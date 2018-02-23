/* Pi-hole: A black hole for Internet advertisements
*  (c) 2017 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  FTL Engine
*  dnsmasq interfacing routines
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "dnsmasq/dnsmasq.h"
#undef __USE_XOPEN
#include "FTL.h"
#include "dnsmasq_interface.h"

void print_flags(unsigned int flags);
void storeIP(int i, char *ip);
void save_reply_type(unsigned int flags, int queryID);

char flagnames[28][12] = {"F_IMMORTAL ", "F_NAMEP ", "F_REVERSE ", "F_FORWARD ", "F_DHCP ", "F_NEG ", "F_HOSTS ", "F_IPV4 ", "F_IPV6 ", "F_BIGNAME ", "F_NXDOMAIN ", "F_CNAME ", "F_DNSKEY ", "F_CONFIG ", "F_DS ", "F_DNSSECOK ", "F_UPSTREAM ", "F_RRNAME ", "F_SERVER ", "F_QUERY ", "F_NOERR ", "F_AUTH ", "F_DNSSEC ", "F_KEYTAG ", "F_SECSTAT ", "F_NO_RR ", "F_IPSET ", "F_NOEXTRA "};

void FTL_new_query(unsigned int flags, char *name, struct all_addr *addr, char *types, int id)
{
	// Create new query in data structure
	enable_thread_lock();
	// Get timestamp
	int querytimestamp, overTimetimestamp;
	gettimestamp(&querytimestamp, &overTimetimestamp);

	if(!config.analyze_AAAA && strcmp(types," query[AAAA]") == 0)
	{
		if(debug) logg("Not analyzing AAAA query");
		disable_thread_lock();
		return;
	}

	// Ensure we have enough space in the queries struct
	memory_check(QUERIES);
	int queryID = counters.queries;
	int timeidx = findOverTimeID(overTimetimestamp);

	// Convert domain to lower case
	char *domain = strdup(name);
	strtolower(domain);

	if(strcmp(domain, "pi.hole") == 0)
	{
		// domain is "pi.hole", skip this query
		// free memory already allocated here
		free(domain);
		disable_thread_lock();
		return;
	}

	// Check and apply possible privacy level rules
	// We do this immediately on the raw data to avoid any possible leaking
	get_privacy_level(NULL);
	if(config.privacylevel >= PRIVACY_HIDE_DOMAINS)
	{
		free(domain);
		domain = strdup("hidden");
	}

	char dest[ADDRSTRLEN];
	inet_ntop((flags & F_IPV4) ? AF_INET : AF_INET6, addr, dest, ADDRSTRLEN);
	char *client = strdup(dest);
	strtolower(client);

	// Check and apply possible privacy level rules
	// We do this immediately on the raw data to avoid any possible leaking
	if(config.privacylevel >= PRIVACY_HIDE_DOMAINS_CLIENTS)
	{
		free(client);
		client = strdup("0.0.0.0");
	}

	if(debug) logg("**** new query %s %s %s (ID %i)", types, domain, client, id);

	unsigned char querytype = 0;
	validate_access("overTime", timeidx, true, __LINE__, __FUNCTION__, __FILE__);
	if(strcmp(types,"query[A]") == 0)
		querytype = TYPE_A;
	else if(strcmp(types,"query[AAAA]") == 0)
		querytype = TYPE_AAAA;
	else if(strcmp(types,"query[ANY]") == 0)
		querytype = TYPE_ANY;
	else if(strcmp(types,"query[SRV]") == 0)
		querytype = TYPE_SRV;
	else if(strcmp(types,"query[SOA]") == 0)
		querytype = TYPE_SOA;
	else if(strcmp(types,"query[PTR]") == 0)
		querytype = TYPE_PTR;
	else if(strcmp(types,"query[TXT]") == 0)
		querytype = TYPE_TXT;
	else
	{
		// Return early to avoid accessing querytypedata out of bounds
		if(debug) logg("Notice: Skipping unknown query type: %s (%i)", types, id);
		free(domain);
		free(client);
		disable_thread_lock();
		return;
	}

	// Update counters
	overTime[timeidx].querytypedata[querytype-1]++;
	counters.querytype[querytype-1]++;

	if(querytype != TYPE_A && querytype != TYPE_AAAA)
	{
		// Don't process this query further here, we already counted it
		if(debug) logg("Notice: Skipping new query: %s (%i)", types, id);
		free(domain);
		free(client);
		disable_thread_lock();
		return;
	}

	// Go through already knows domains and see if it is one of them
	// Check struct size
	memory_check(DOMAINS);
	int domainID = findDomainID(domain);

	// Go through already knows clients and see if it is one of them
	// Check struct size
	memory_check(CLIENTS);
	int clientID = findClientID(client);

	// Save everything
	validate_access("queries", queryID, false, __LINE__, __FUNCTION__, __FILE__);
	queries[queryID].magic = MAGICBYTE;
	queries[queryID].timestamp = querytimestamp;
	queries[queryID].type = querytype;
	queries[queryID].status = QUERY_UNKNOWN;
	queries[queryID].domainID = domainID;
	queries[queryID].clientID = clientID;
	queries[queryID].timeidx = timeidx;
	queries[queryID].db = false;
	queries[queryID].id = id;
	queries[queryID].complete = false;
	queries[queryID].private = (config.privacylevel == PRIVACY_MAXIMUM);
	queries[queryID].ttl = 0;

	// Increase DNS queries counter
	counters.queries++;
	// Count this query as unknown as long as no reply has
	// been found and analyzed
	counters.unknown++;

	// Update overTime data
	validate_access("overTime", timeidx, true, __LINE__, __FUNCTION__, __FILE__);
	overTime[timeidx].total++;

	// Update overTime data structure with the new client
	validate_access_oTcl(timeidx, clientID, __LINE__, __FUNCTION__, __FILE__);
	overTime[timeidx].clientdata[clientID]++;

	// Free allocated memory
	free(client);
	free(domain);
	disable_thread_lock();
}

void FTL_forwarded(unsigned int flags, char *name, struct all_addr *addr, int id)
{
	// Save that this query got forwarded to an updtream server
	enable_thread_lock();
	char dest[ADDRSTRLEN];
	inet_ntop(flags & F_IPV4 ? AF_INET : AF_INET6, addr, dest, ADDRSTRLEN);

	if(debug) logg("**** forwarded %s to %s (ID %i)", name, dest, id);

	// Convert forward to lower case
	char *forward = strdup(dest);
	strtolower(forward);

	// Save status and forwardID in corresponding query indentified by dnsmasq's ID
	bool found = false;
	int i;
	for(i=0; i<counters.queries; i++)
	{
		validate_access("queries", i, false, __LINE__, __FUNCTION__, __FILE__);
		// Check UUID of this query
		if(queries[i].id == id)
		{
			// Detect if we cached the <CNAME> but need to ask the upstream
			// servers for the actual IPs now
			if(queries[i].status == QUERY_CACHE)
			{
				// Fix counters
				counters.cached--;
				validate_access("overTime", queries[i].timeidx, true, __LINE__, __FUNCTION__, __FILE__);
				overTime[queries[i].timeidx].cached--;

				// Mark this query again as (temporarily) unknown
				counters.unknown++;
				queries[i].complete = false;
			}
			queries[i].status = QUERY_FORWARDED;
			found = true;
			break;
		}
	}
	if(!found)
	{
		// This may happen e.g. if the original query was a PTR query or "pi.hole"
		// as we ignore them altogether
		free(forward);
		disable_thread_lock();
		return;
	}

	// Count only if current query has not been counted so far
	if(queries[i].complete)
	{
		free(forward);
		disable_thread_lock();
		return;
	}

	// Get ID of forward destination, create new forward destination record
	// if not found in current data structure
	int forwardID = findForwardID(forward, true);
	queries[i].forwardID = forwardID;

	if(!queries[i].complete)
	{
		// This query is no longer unknown ...
		counters.unknown--;
		// ... but got forwarded
		counters.forwardedqueries++;
		// Hereby, this query is now fully determined
		queries[i].complete = true;
	}

	// Release allocated memory
	free(forward);
	disable_thread_lock();
}

void FTL_dnsmasq_reload(void)
{
	// Called when dnsmasq re-reads its config and hosts files
	// Reset number of blocked domains and re-read list of wildcard domains
	counters.gravity = 0;
	readWildcardsList();
}

void FTL_read_hosts(char * filename, int addr_count)
{
	// Interpret hosts files that have been read by dnsmasq
	// We ignore other read lists like /etc/hosts
	enable_thread_lock();
	if(filename != NULL && (strstr(filename, "/gravity.list") != NULL ||
	                        strstr(filename, "/black.list") != NULL))
	{
		counters.gravity += addr_count;
	}
	disable_thread_lock();
}

void FTL_reply(unsigned short flags, char *name, struct all_addr *addr, unsigned long ttl, int id)
{
	// Interpret hosts files that have been read by dnsmasq
	enable_thread_lock();
	// Determine returned result if available
	char dest[ADDRSTRLEN]; dest[0] = '\0';
	if(addr)
	{
		inet_ntop(flags & F_IPV4 ? AF_INET : AF_INET6, addr, dest, ADDRSTRLEN);
	}

	if(debug)
	{
		char *answer = dest;
		if(flags & F_CNAME)
			answer = "(CNAME)";
		else if(flags & F_NEG && flags & F_NXDOMAIN)
			answer = "(NXDOMAIN)";
		else if(flags & F_NEG)
			answer = "(NODATA)";

		logg("**** got reply %s is %s (TTL %lu, ID %i)", name, answer, ttl, id);
		print_flags(flags);
	}

	if(flags & F_CONFIG)
	{
		// Answered from local configuration, might be a wildcard or user-provided
		// Save status in corresponding query indentified by dnsmasq's ID
		bool found = false;
		int i;
		for(i=0; i<counters.queries; i++)
		{
			validate_access("queries", i, false, __LINE__, __FUNCTION__, __FILE__);
			// Check UUID of this query
			if(queries[i].id == id)
			{
				queries[i].status = detectStatus(domains[queries[i].domainID].domain);
				found = true;
				break;
			}
		}

		if(!found)
		{
			// This may happen e.g. if the original query was a PTR query or "pi.hole"
			// as we ignore them altogether
			disable_thread_lock();
			return;
		}

		if(!queries[i].complete)
		{
			// This query is no longer unknown
			counters.unknown--;

			// Get time index
			int querytimestamp, overTimetimestamp;
			gettimestamp(&querytimestamp, &overTimetimestamp);
			int timeidx = findOverTimeID(overTimetimestamp);

			int domainID = queries[i].domainID;
			validate_access("domains", domainID, true, __LINE__, __FUNCTION__, __FILE__);

			// Decide what to do depening on the result of detectStatus()
			if(queries[i].status == QUERY_WILDCARD)
			{
				// Blocked due to a matching wildcard rule
				counters.wildcardblocked++;

				validate_access("overTime", timeidx, true, __LINE__, __FUNCTION__, __FILE__);
				overTime[timeidx].blocked++;
				domains[queries[i].domainID].blockedcount++;
				domains[queries[i].domainID].wildcard = true;
			}
			else if(queries[i].status == QUERY_CACHE)
			{
				// Answered from a custom (user provided) cache file
				counters.cached++;

				validate_access("overTime", timeidx, true, __LINE__, __FUNCTION__, __FILE__);
				overTime[timeidx].cached++;
			}

			// Save reply type and update individual reply counters
			save_reply_type(flags, i);

			// Save returned IP
			// but only if we have an exact match (there will be no exact match for a CNAME)
			// on this else-branch this is not a negative match (NXDOMAIN, NODATA)
			if(!(flags & F_NEG) && !(flags & F_CNAME) &&
			   strlen(dest) > 2 &&
			   strcmp(domains[domainID].domain, name) == 0)
			{
				storeIP(i, dest);
			}

			// Store TTL
			queries[i].ttl = ttl;

			// Hereby, this query is now fully determined
			queries[i].complete = true;
		}

		// We are done here
		disable_thread_lock();
		return;
	}
	else if(flags & F_FORWARD)
	{
		// Search for corresponding query indentified by dnsmasq's ID
		bool found = false;
		int i;
		for(i=0; i<counters.queries; i++)
		{
			validate_access("queries", i, false, __LINE__, __FUNCTION__, __FILE__);
			// Check UUID of this query
			if(queries[i].id == id)
			{
				found = true;
				break;
			}
		}

		if(!found)
		{
			// This may happen e.g. if the original query was a PTR query or "pi.hole"
			// as we ignore them altogether
			disable_thread_lock();
			return;
		}

		int domainID = queries[i].domainID;
		validate_access("domains", domainID, true, __LINE__, __FUNCTION__, __FILE__);
		if(strcmp(domains[domainID].domain, name) == 0)
		{

			// Save reply type and update individual reply counters
			save_reply_type(flags, i);

			// Save returned IP
			// but only if we have an exact match (there will be no exact match for a CNAME)
			// on this else-branch this is not a negative match (NXDOMAIN, NODATA)
			if(!(flags & F_NEG) && !(flags & F_CNAME) &&
			   strlen(dest) > 2)
			{
				storeIP(i, dest);
			}

			// Store TTL
			queries[i].ttl = ttl;
		}
	}
	else
	{
		logg("*************************** unknown REPLY ***************************");
		print_flags(flags);
	}
	disable_thread_lock();
}

void FTL_cache(unsigned int flags, char *name, struct all_addr *addr, char *arg, unsigned long ttl, int id)
{
	// Save that this query got answered from cache
	enable_thread_lock();
	char dest[ADDRSTRLEN]; dest[0] = '\0';
	if(addr)
	{
		inet_ntop(flags & F_IPV4 ? AF_INET : AF_INET6, addr, dest, ADDRSTRLEN);
	}

	// Convert domain to lower case
	char *domain = strdup(name);
	strtolower(domain);
	if(strcmp(domain, "pi.hole") == 0)
	{
		// domain is "pi.hole", skip this query
		// free memory already allocated here
		free(domain);
		disable_thread_lock();
		return;
	}
	free(domain);

	if(debug) logg("**** got cache answer for %s / %s / %s (TTL %lu, ID %i)", name, dest, arg, ttl, id);
	if(debug) print_flags(flags);

	if(((flags & F_HOSTS) && (flags & F_IMMORTAL)) || ((flags & F_NAMEP) && (flags & F_DHCP)) || (flags & F_FORWARD))
	{
		// Hosts data: /etc/pihole/gravity.list, /etc/pihole/black.list, /etc/pihole/local.list, etc.
		// or
		// DHCP server reply
		// or
		// cached answer to previously forwarded request

		// Determine requesttype
		unsigned char requesttype = 0;
		if(flags & F_HOSTS)
		{
			if(arg != NULL && strstr(arg, "/gravity.list") != NULL)
				requesttype = QUERY_GRAVITY;
			else if(arg != NULL && strstr(arg, "/black.list") != NULL)
				requesttype = QUERY_BLACKLIST;
			else // local.list, hostname.list, /etc/hosts and others
				requesttype = QUERY_CACHE;
		}
		else if((flags & F_NAMEP) && (flags & F_DHCP)) // DHCP server reply
			requesttype = QUERY_CACHE;
		else if(flags & F_FORWARD) // cached answer to previously forwarded request
			requesttype = QUERY_CACHE;
		else
		{
			logg("*************************** unknown CACHE reply (1) ***************************");
			print_flags(flags);
		}

		bool found = false;
		int i;
		for(i=0; i<counters.queries; i++)
		{
			validate_access("queries", i, false, __LINE__, __FUNCTION__, __FILE__);
			// Check UUID of this query
			if(queries[i].id == id)
			{
				queries[i].status = requesttype;
				found = true;
				break;
			}
		}
		if(!found)
		{
			// This may happen e.g. if the original query was a PTR query or "pi.hole"
			// as we ignore them altogether
			disable_thread_lock();
			return;
		}

		int domainID = queries[i].domainID;
		validate_access("domains", domainID, true, __LINE__, __FUNCTION__, __FILE__);
		if(!queries[i].complete)
		{
			// This query is no longer unknown
			counters.unknown--;

			// Get time index
			int querytimestamp, overTimetimestamp;
			gettimestamp(&querytimestamp, &overTimetimestamp);
			int timeidx = findOverTimeID(overTimetimestamp);
			validate_access("overTime", timeidx, true, __LINE__, __FUNCTION__, __FILE__);

			// Handle counters accordingly
			switch(requesttype)
			{
				case QUERY_GRAVITY: // gravity.list
				case QUERY_BLACKLIST: // black.list
					counters.blocked++;
					overTime[timeidx].blocked++;
					domains[domainID].blockedcount++;
					break;
				case QUERY_CACHE: // cached from one of the lists
					counters.cached++;
					overTime[timeidx].cached++;
					break;
			}

			// Save reply type and update individual reply counters
			save_reply_type(flags, i);

			// Save returned IP
			// but only if we have an exact match (there will be no exact match for a CNAME)
			// on this else-branch this is not a negative match (NXDOMAIN, NODATA)
			if(!(flags & F_NEG) && !(flags & F_CNAME) &&
			   strlen(dest) > 2 &&
			   strcmp(domains[domainID].domain, name) == 0)
			{
				storeIP(i, dest);
			}

			// Store TTL
			queries[i].ttl = ttl;

			// Hereby, this query is now fully determined
			queries[i].complete = true;
		}
	}
	else
	{
		logg("*************************** unknown CACHE reply (2) ***************************");
		print_flags(flags);
	}
	disable_thread_lock();
}

void storeIP(int i, char *ip)
{
	validate_access("queries", i, true, __LINE__, __FUNCTION__, __FILE__);
	int domainID = queries[i].domainID;
	validate_access("domains", domainID, true, __LINE__, __FUNCTION__, __FILE__);
	if(queries[i].type == TYPE_A) // IPv4 query
	{
		// First check if entry is already set
		if(domains[domainID].IPv4 != NULL)
		{
			if(strcmp(domains[domainID].IPv4, ip) != 0)
			{
				free(domains[domainID].IPv4);
				domains[domainID].IPv4 = strdup(ip);
			}
		}
		else
		{
			domains[domainID].IPv4 = strdup(ip);
		}
	}
	else if(queries[i].type == TYPE_AAAA) // IPv6 query
	{
		// First check if entry is already set
		if(domains[domainID].IPv6 != NULL)
		{
			if(strcmp(domains[domainID].IPv6, ip) != 0)
			{
				free(domains[domainID].IPv6);
				domains[domainID].IPv6 = strdup(ip);
			}
		}
		else
		{
			domains[domainID].IPv6 = strdup(ip);
		}
	}
}

void FTL_dnssec(int status, int id)
{
	// Process DNSSEC result for a domain
	enable_thread_lock();
	// Search for corresponding query indentified by ID
	bool found = false;
	int i;
	for(i=0; i<counters.queries; i++)
	{
		// Check both UUID and generation of this query
		if(queries[i].id == id)
		{
			found = true;
			break;
		}
	}

	if(!found)
	{
		// This may happen e.g. if the original query was an unhandled query type
		disable_thread_lock();
		return;
	}
	validate_access("domains", queries[i].domainID, true, __LINE__, __FUNCTION__, __FILE__);
	if(debug) logg("**** got DNSSEC details for %s: %i (ID %i)", domains[queries[i].domainID].domain, status, id);

	// Iterate through possible values
	if(status == STAT_SECURE)
		domains[queries[i].domainID].dnssec = DNSSEC_SECURE;
	else if(status == STAT_INSECURE)
		domains[queries[i].domainID].dnssec = DNSSEC_INSECURE;
	else
		domains[queries[i].domainID].dnssec = DNSSEC_BOGUS;

	disable_thread_lock();
}

void print_flags(unsigned int flags)
{
	unsigned int i;
	char *flagstr = calloc(256,sizeof(char));
	for(i = 0; i < sizeof(flags)*8; i++)
		if(flags & (1 << i))
			strcat(flagstr, flagnames[i]);
	logg("     Flags: %s", flagstr);
	free(flagstr);
}

void save_reply_type(unsigned int flags, int queryID)
{
	// Iterate through possible values
	validate_access("queries", queryID, false, __LINE__, __FUNCTION__, __FILE__);
	int domainID = queries[queryID].domainID;
	validate_access("domains", domainID, false, __LINE__, __FUNCTION__, __FILE__);
	int replyID = queries[queryID].type == TYPE_A ? 0 : 1;
	if(flags & F_NEG)
	{
		if(flags & F_NXDOMAIN)
		{
			// NXDOMAIN
			domains[domainID].reply[replyID] = REPLY_NXDOMAIN;
			counters.reply_NXDOMAIN++;
		}
		else
		{
			// NODATA(-IPv6)
			domains[domainID].reply[replyID] = REPLY_NODATA;
			counters.reply_NODATA++;
		}
	}
	else if(flags & F_CNAME)
	{
		// <CNAME>
		domains[domainID].reply[replyID] = REPLY_CNAME;
		counters.reply_CNAME++;
	}
	else
	{
		// Valid IP
		domains[domainID].reply[replyID] = REPLY_IP;
		counters.reply_IP++;
	}
}