/**
 * @file
 * @brief Simple pcap interface code.
 *
 *
 * This file is part of the Assimilation Project.
 *
 * @author Copyright &copy; 2011, 2012 - Alan Robertson <alanr@unix.sh>
 * @n
 *  The Assimilation software is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  The Assimilation software is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with the Assimilation Project software.  If not, see http://www.gnu.org/licenses/
 *  @todo In general, need to exclude sent packets from received packets even on those platforms
 *  (like Linux) where libpcap won't filter that for us.  This will probably involve filtering by source
 *  MAC address.
 *
 *  @todo To figure out what the MAC address of an interface on Windows is, 
 *  use the GetAdapterAddresses function - http://msdn.microsoft.com/en-us/library/aa365915(v=vs.85).aspx
 * @todo convert all the messaging over to use the various glib logging functions.
 *
 */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <projectcommon.h>
#include <cdp.h>
#include <lldp.h>
#include <pcap_min.h>
#include <addrframe.h>
#include <proj_classes.h>

#define DIMOF(a)	(sizeof(a)/sizeof(a[0]))


/// Structure mapping @ref pcap_protocols bits to the corresponding pcap filter expressions.
static struct pcap_filter_info {
	const unsigned	filterbit;
	const char *	filter;
	const char *	mcastaddr;
}filterinfo[] = {
	{ENABLE_LLDP,	"(ether proto 0x88cc and ether dst 01:80:c2:00:00:0e)",   	"01:80:c2:00:00:0e"},
	//{ENABLE_CDP,	"(ether dst 01:00:0c:cc:cc:cc and ether[20:2] == 0x2000)",	"01:00:0c:cc:cc:cc"},
	{ENABLE_CDP,	"(ether dst 01:00:0c:cc:cc:cc)",				"01:00:0c:cc:cc:cc"},
	{ENABLE_ARP,	"(arp)", NULL},

};

DEBUGDECLARATIONS
FSTATIC gboolean _enable_mcast_address(const char * addrstring, const char * dev, gboolean enable);

/**
 *  Set up pcap listener for the given interfaces and protocols.
 *  @return a properly configured pcap_t* object for listening for the given protocols - NULL on error
 *  @see pcap_protocols
 */
pcap_t*
create_pcap_listener(const char * dev		///<[in] Device name to listen on
,		     gboolean blocking		///<[in] TRUE if this is a blocking connection
,		     unsigned listenmask	///<[in] Bit mask of protocols to listen for
						///< (see @ref pcap_protocols "list of valid bits")
,		     struct bpf_program*prog)	///<[out] Compiled PCAP program
{
	pcap_t*			pcdescr = NULL;
	bpf_u_int32		maskp = 0;
	bpf_u_int32		netp = 0;
	char			errbuf[PCAP_ERRBUF_SIZE];
	char *			expr = NULL;
	int			filterlen = 1;
	unsigned		j;
	int			cnt=0;
	int			rc;
	const char ORWORD [] = " or ";
	gboolean		need_promisc = FALSE;

	BINDDEBUG(pcap_t);
//	setbuf(stdout, NULL);
	setvbuf(stdout, NULL, _IONBF, 0);
	errbuf[0] = '\0';

	// Search the list of valid bits so we can construct the libpcap filter
	// for the given set of protocols on the fly...
	// On this pass we just compute the amount of memory we'll need...
	for (j = 0, cnt = 0; j < DIMOF(filterinfo); ++j) {
		if (listenmask & filterinfo[j].filterbit) {
			++cnt;
			if (cnt > 1) {
				filterlen += sizeof(ORWORD);
			}
			filterlen += strlen(filterinfo[j].filter);
		}
	}

	if (filterlen < 2) {
		g_warning("Constructed filter is too short - invalid mask argument.");
		return NULL;
	}
	if (NULL == (expr = malloc(filterlen))) {
		g_error("Out of memory!");
		return NULL;
	}
	// Same song, different verse...
	// This time around, we construct the filter
	expr[0] = '\0';
	for (j = 0, cnt = 0; j < DIMOF(filterinfo); ++j) {
		if (listenmask & filterinfo[j].filterbit) {
			++cnt;
			if (cnt > 1) {
				g_strlcat(expr, ORWORD, filterlen);
			}
			g_strlcat(expr, filterinfo[j].filter, filterlen);
		}
	}
	if (pcap_lookupnet(dev, &netp, &maskp, errbuf) != 0) {
		// This is not a problem for non-IPv4 protocols...
		// It just looks up the ipv4 address - which we mostly don't care about.
		g_info("%s.%d: pcap_lookupnet(\"%s\") failed: [%s]"
		,	__FUNCTION__, __LINE__, dev, errbuf);
	}

	if (NULL == (pcdescr = pcap_create(dev, errbuf))) {
		g_warning("pcap_create failed: [%s]", errbuf);
		goto oopsie;
	}
	//pcap_set_promisc(pcdescr, FALSE);
	for (j = 0; j < DIMOF(filterinfo); ++j) {
		if (listenmask & filterinfo[j].filterbit) {
			const char * addrstring = filterinfo[j].mcastaddr;
			if (addrstring && !_enable_mcast_address(addrstring, dev, TRUE)) {
				need_promisc = TRUE;
			}
		}
	}
	pcap_set_promisc(pcdescr, need_promisc);
#ifdef HAVE_PCAP_SET_RFMON
	pcap_set_rfmon(pcdescr, FALSE);
#endif
	pcap_setdirection(pcdescr, PCAP_D_IN);
	// Weird bug - returns -3 and doesn't show an error message...
	// And pcap_getnonblock also returns -3... Neither should happen AFAIK...
	errbuf[0] = '\0';
	if ((rc = pcap_setnonblock(pcdescr, !blocking, errbuf)) < 0 && errbuf[0] != '\0') {
		g_warning("pcap_setnonblock(%d) failed: [%s] [rc=%d]", !blocking, errbuf, rc);
		g_warning("Have no idea why this happens - current blocking state is: %d."
		,	pcap_getnonblock(pcdescr, errbuf));
	}
	pcap_set_snaplen(pcdescr, 1500);
	/// @todo deal with pcap_set_timeout() call here.
	if (blocking) {
		pcap_set_timeout(pcdescr, 240*1000);
	}else{
		pcap_set_timeout(pcdescr, 1);
	}
	//pcap_set_buffer_size(pcdescr, 1500);
      
	if (pcap_activate(pcdescr) != 0) {
		g_warning("pcap_activate failed: [%s]", pcap_geterr(pcdescr));
		goto oopsie;
	}
	if (pcap_compile(pcdescr, prog, expr, FALSE, maskp) < 0) {
		g_warning("pcap_compile of [%s] failed: [%s]", expr, pcap_geterr(pcdescr));
		goto oopsie;
	}
	if (pcap_setfilter(pcdescr, prog) < 0) {
		g_warning("pcap_setfilter on [%s] failed: [%s]", expr, pcap_geterr(pcdescr));
		goto oopsie;
	}
	DEBUGMSG1("Compile of [%s] worked!", expr);
	free(expr); expr = NULL;
	return(pcdescr);

oopsie:	// Some kind of failure - free things up and return NULL

	g_warning("%s.%d: Could not set up PCAP on %s"
	,	__FUNCTION__, __LINE__, dev);
	if (expr) {
		free(expr);
		expr = NULL;
	}
	if (pcdescr) {
		close_pcap_listener(pcdescr, dev, listenmask);
		pcdescr = NULL;
	}
	return NULL;
}

/// Close this pcap_listener, and undo listens for multicast addresses
void
close_pcap_listener(pcap_t*	pcapdev		///< Pcap device structure
,		    const char*	dev		///< device that this is opened on
,		    unsigned	listenmask)	///< The 'listenmask' given to create_pcap_listener
{
	unsigned j;
	pcap_close(pcapdev);
	for (j = 0; j < DIMOF(filterinfo); ++j) {
		if (listenmask & filterinfo[j].filterbit && filterinfo[j].mcastaddr) {
			_enable_mcast_address(filterinfo[j].mcastaddr, dev, FALSE);
		}
	}
}


/// Function to enable listening to a particular ethernet multicast address.
/// This is a highly non-portable function.
/// I wonder how you do this on BSD or Slowlaris?
FSTATIC gboolean
_enable_mcast_address(const char * addrstring	///<[in] multicast MAC address string suitable for giving to 'ip'
,		     const char * dev		///<[in] ethernet device
,		     gboolean enable)		///<[in] TRUE to enable, FALSE to disable
{
	GSpawnFlags	flags =  G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL | G_SPAWN_SEARCH_PATH;
	gint		exit_status;
	const gchar*	constargv [] =
	{"ip", "maddress", (enable ? "add" : "delete"), addrstring, "dev", dev, NULL};
	gchar*		argv[DIMOF(constargv)];
	unsigned	j;


	if (NULL == addrstring) {
		return FALSE;
	}

	// This is really stupid and annoying - they have the wrong function prototype for g_spawn_sync...
	for (j=0; j < DIMOF(argv); ++j) {
		argv[j] = g_strdup(constargv[j]);
	}

	DEBUGMSG1("Running IP command %s %s %s %s %s %s", argv[0], argv[1], argv[2], argv[3], argv[4], argv[5]);
	if (!g_spawn_sync(NULL, argv, NULL, flags, NULL, NULL, NULL, NULL, &exit_status, NULL)) {
		exit_status = 300;
	}
	for (j=0; j < DIMOF(argv); ++j) {
		g_free(argv[j]);
		argv[j] = NULL;
	}
	DEBUGMSG1("Previous IP command returned %d", exit_status);
	return exit_status == 0;
}

// Create an iterator object for iterating over libpcap-captured packets
// These are pcap_capture files are primarily for writing tests in Python
// We have to do this testing in Python, because our C code just encapsulates them
// and passes them in binary to the CMA. It does this so we don't have to distribute
// all the smarts about these packets, and their extensions out to all the nanoprobes.
WINEXPORT struct pcap_capture_iter*
pcap_capture_iter_new(const char* capture_filename)
{
	pcap_t*	pcfd;
	struct pcap_capture_iter *	iter;
	char	errbuf[PCAP_ERRBUF_SIZE];
	errbuf[0] = '\0';
	pcfd = pcap_open_offline(capture_filename, errbuf);
	if (NULL == pcfd) {
		g_warning("%s.%d: cannot open capture file %s: %s"
		,	__FUNCTION__, __LINE__, capture_filename, errbuf);
		return NULL;
	}
	iter = MALLOCTYPE(struct pcap_capture_iter);
	iter->pcfd = pcfd;
	return iter;
}

// Free (destroy) an iterator object for iterating over libpcap-captured packets
WINEXPORT void
pcap_capture_iter_del(struct pcap_capture_iter* iter)
{
	if (iter != NULL && iter->pcfd != NULL) {
		pcap_close(iter->pcfd);
		iter->pcfd = NULL;
		FREE(iter);
	}
}

// Return the next libpcap-captured packet from our iterator.
// We return NULL if there are no more packets in the capture.
WINEXPORT const guint8*
pcap_capture_iter_next(struct pcap_capture_iter* iter, const guint8** pktend, guint* pktlen)
{
	const guint8*		pkt;
	struct pcap_pkthdr*	hdr;

	if (pcap_next_ex(iter->pcfd, &hdr, &pkt) < 0) {
		*pktlen = 0;
		*pktend = NULL;
		return NULL;
	}
	*pktlen = hdr->caplen;
	*pktend = pkt + hdr->caplen;
	return pkt;
}
