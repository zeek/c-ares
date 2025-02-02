/* MIT License
 *
 * Copyright (c) 1998 Massachusetts Institute of Technology
 * Copyright (c) 2007 Daniel Stenberg
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * SPDX-License-Identifier: MIT
 */

#include "ares_setup.h"

#ifdef HAVE_SYS_PARAM_H
#  include <sys/param.h>
#endif

#ifdef HAVE_NETINET_IN_H
#  include <netinet/in.h>
#endif

#ifdef HAVE_NETDB_H
#  include <netdb.h>
#endif

#ifdef HAVE_ARPA_INET_H
#  include <arpa/inet.h>
#endif

#include "ares_nameser.h"

#if defined(ANDROID) || defined(__ANDROID__)
#  include <sys/system_properties.h>
#  include "ares_android.h"
/* From the Bionic sources */
#  define DNS_PROP_NAME_PREFIX "net.dns"
#  define MAX_DNS_PROPERTIES   8
#endif

#if defined(CARES_USE_LIBRESOLV)
#  include <resolv.h>
#endif

#if defined(USE_WINSOCK)
#  include <iphlpapi.h>
#endif

#include "ares.h"
#include "ares_inet_net_pton.h"
#include "ares_platform.h"
#include "ares_private.h"

#ifdef WATT32
#  undef WIN32 /* Redefined in MingW/MSVC headers */
#endif


static ares_status_t init_by_environment(ares_channel channel);
static ares_status_t init_by_resolv_conf(ares_channel channel);
static ares_status_t init_by_defaults(ares_channel channel);

#ifndef WATT32
static ares_status_t config_nameserver(struct server_state **servers,
                                       size_t *nservers, const char *str);
#endif
static ares_status_t set_search(ares_channel channel, const char *str);
static ares_status_t set_options(ares_channel channel, const char *str);
static const char   *try_option(const char *p, const char *q, const char *opt);

static ares_status_t config_sortlist(struct apattern **sortlist, size_t *nsort,
                                     const char *str);
static ares_bool_t   sortlist_alloc(struct apattern **sortlist, size_t *nsort,
                                    struct apattern *pat);
static int  ip_addr(const char *s, ares_ssize_t len, struct in_addr *addr);
static void natural_mask(struct apattern *pat);
#if !defined(WIN32) && !defined(WATT32) && !defined(ANDROID) && \
  !defined(__ANDROID__) && !defined(CARES_USE_LIBRESOLV)
static ares_status_t config_domain(ares_channel channel, char *str);
static ares_status_t config_lookup(ares_channel channel, const char *str,
                                   const char *bindch, const char *altbindch,
                                   const char *filech);
static char         *try_config(char *s, const char *opt, char scc);
#endif



int ares_init(ares_channel *channelptr)
{
  return ares_init_options(channelptr, NULL, 0);
}

static int ares_query_timeout_cmp_cb(const void *arg1, const void *arg2)
{
  const struct query *q1 = arg1;
  const struct query *q2 = arg2;

  if (q1->timeout.tv_sec > q2->timeout.tv_sec) {
    return 1;
  }
  if (q1->timeout.tv_sec < q2->timeout.tv_sec) {
    return -1;
  }

  if (q1->timeout.tv_usec > q2->timeout.tv_usec) {
    return 1;
  }
  if (q1->timeout.tv_usec < q2->timeout.tv_usec) {
    return -1;
  }

  return 0;
}

int ares_init_options(ares_channel *channelptr, struct ares_options *options,
                      int optmask)
{
  ares_channel  channel;
  ares_status_t status = ARES_SUCCESS;

  if (ares_library_initialized() != ARES_SUCCESS) {
    return ARES_ENOTINITIALIZED; /* LCOV_EXCL_LINE: n/a on non-WinSock */
  }

  channel = ares_malloc(sizeof(*channel));
  if (!channel) {
    *channelptr = NULL;
    return ARES_ENOMEM;
  }

  memset(channel, 0, sizeof(*channel));

  /* Generate random key */

  channel->rand_state = ares__init_rand_state();
  if (channel->rand_state == NULL) {
    status = ARES_ENOMEM;
    DEBUGF(fprintf(stderr, "Error: init_id_key failed: %s\n",
                   ares_strerror(status)));
    goto done;
  }

  /* Initialize our lists of queries */
  channel->all_queries = ares__llist_create(NULL);
  if (channel->all_queries == NULL) {
    status = ARES_ENOMEM;
    goto done;
  }

  channel->queries_by_qid = ares__htable_szvp_create(NULL);
  if (channel->queries_by_qid == NULL) {
    status = ARES_ENOMEM;
    goto done;
  }

  channel->queries_by_timeout =
    ares__slist_create(channel->rand_state, ares_query_timeout_cmp_cb, NULL);
  if (channel->queries_by_timeout == NULL) {
    status = ARES_ENOMEM;
    goto done;
  }

  channel->connnode_by_socket = ares__htable_asvp_create(NULL);
  if (channel->connnode_by_socket == NULL) {
    status = ARES_ENOMEM;
    goto done;
  }

  /* Initialize configuration by each of the four sources, from highest
   * precedence to lowest.
   */

  status = ares__init_by_options(channel, options, optmask);
  if (status != ARES_SUCCESS) {
    DEBUGF(fprintf(stderr, "Error: init_by_options failed: %s\n",
                   ares_strerror(status)));
    /* If we fail to apply user-specified options, fail the whole init process
     */
    goto done;
  }
  status = init_by_environment(channel);
  if (status != ARES_SUCCESS) {
    DEBUGF(fprintf(stderr, "Error: init_by_environment failed: %s\n",
                   ares_strerror(status)));
  }
  if (status == ARES_SUCCESS) {
    status = init_by_resolv_conf(channel);
    if (status != ARES_SUCCESS) {
      DEBUGF(fprintf(stderr, "Error: init_by_resolv_conf failed: %s\n",
                     ares_strerror(status)));
    }
  }

  /*
   * No matter what failed or succeeded, seed defaults to provide
   * useful behavior for things that we missed.
   */
  status = init_by_defaults(channel);
  if (status != ARES_SUCCESS) {
    DEBUGF(fprintf(stderr, "Error: init_by_defaults failed: %s\n",
                   ares_strerror(status)));
  }

  /* Trim to one server if ARES_FLAG_PRIMARY is set. */
  if ((channel->flags & ARES_FLAG_PRIMARY) && channel->nservers > 1) {
    channel->nservers = 1;
  }

  status = ares__init_servers_state(channel);
  if (status != ARES_SUCCESS) {
    goto done;
  }

done:
  if (status != ARES_SUCCESS) {
    /* Something failed; clean up memory we may have allocated. */
    if (channel->servers) {
      ares_free(channel->servers);
    }
    if (channel->ndomains > 0) {
      ares__strsplit_free(channel->domains, channel->ndomains);
    }
    if (channel->sortlist) {
      ares_free(channel->sortlist);
    }
    if (channel->lookups) {
      ares_free(channel->lookups);
    }
    if (channel->resolvconf_path) {
      ares_free(channel->resolvconf_path);
    }
    if (channel->hosts_path) {
      ares_free(channel->hosts_path);
    }
    if (channel->rand_state) {
      ares__destroy_rand_state(channel->rand_state);
    }

    ares__htable_szvp_destroy(channel->queries_by_qid);
    ares__llist_destroy(channel->all_queries);
    ares__slist_destroy(channel->queries_by_timeout);
    ares__htable_asvp_destroy(channel->connnode_by_socket);
    ares_free(channel);
    return (int)status;
  }

  *channelptr = channel;
  return ARES_SUCCESS;
}

/* ares_dup() duplicates a channel handle with all its options and returns a
   new channel handle */
int ares_dup(ares_channel *dest, ares_channel src)
{
  struct ares_options         opts;
  struct ares_addr_port_node *servers;
  int                         non_v4_default_port = 0;
  size_t                      i;
  ares_status_t               rc;
  int                         optmask;

  *dest = NULL; /* in case of failure return NULL explicitly */

  /* First get the options supported by the old ares_save_options() function,
     which is most of them */
  rc = (ares_status_t)ares_save_options(src, &opts, &optmask);
  if (rc) {
    ares_destroy_options(&opts);
    return (int)rc;
  }

  /* Then create the new channel with those options */
  rc = (ares_status_t)ares_init_options(dest, &opts, optmask);

  /* destroy the options copy to not leak any memory */
  ares_destroy_options(&opts);

  if (rc) {
    return (int)rc;
  }

  /* Now clone the options that ares_save_options() doesn't support. */
  (*dest)->sock_create_cb      = src->sock_create_cb;
  (*dest)->sock_create_cb_data = src->sock_create_cb_data;
  (*dest)->sock_config_cb      = src->sock_config_cb;
  (*dest)->sock_config_cb_data = src->sock_config_cb_data;
  (*dest)->sock_funcs          = src->sock_funcs;
  (*dest)->sock_func_cb_data   = src->sock_func_cb_data;

  ares_strcpy((*dest)->local_dev_name, src->local_dev_name,
              sizeof((*dest)->local_dev_name));
  (*dest)->local_ip4 = src->local_ip4;
  memcpy((*dest)->local_ip6, src->local_ip6, sizeof(src->local_ip6));

  /* Full name server cloning required if there is a non-IPv4, or non-default
   * port, nameserver */
  for (i = 0; i < src->nservers; i++) {
    if ((src->servers[i].addr.family != AF_INET) ||
        (src->servers[i].addr.udp_port != 0) ||
        (src->servers[i].addr.tcp_port != 0)) {
      non_v4_default_port++;
      break;
    }
  }
  if (non_v4_default_port) {
    rc = (ares_status_t)ares_get_servers_ports(src, &servers);
    if (rc != ARES_SUCCESS) {
      ares_destroy(*dest);
      *dest = NULL;
      return (int)rc;
    }
    rc = (ares_status_t)ares_set_servers_ports(*dest, servers);
    ares_free_data(servers);
    if (rc != ARES_SUCCESS) {
      ares_destroy(*dest);
      *dest = NULL;
      return (int)rc;
    }
  }

  return ARES_SUCCESS; /* everything went fine */
}


static ares_status_t init_by_environment(ares_channel channel)
{
  const char   *localdomain;
  const char   *res_options;
  ares_status_t status;

  localdomain = getenv("LOCALDOMAIN");
  if (localdomain && channel->ndomains == 0) {
    status = set_search(channel, localdomain);
    if (status != ARES_SUCCESS) {
      return status;
    }
  }

  res_options = getenv("RES_OPTIONS");
  if (res_options) {
    status = set_options(channel, res_options);
    if (status != ARES_SUCCESS) {
      return status; /* LCOV_EXCL_LINE: set_options() never fails */
    }
  }

  return ARES_SUCCESS;
}

#ifdef WIN32
/*
 * get_REG_SZ()
 *
 * Given a 'hKey' handle to an open registry key and a 'leafKeyName' pointer
 * to the name of the registry leaf key to be queried, fetch it's string
 * value and return a pointer in *outptr to a newly allocated memory area
 * holding it as a null-terminated string.
 *
 * Returns 0 and nullifies *outptr upon inability to return a string value.
 *
 * Returns 1 and sets *outptr when returning a dynamically allocated string.
 *
 * Supported on Windows NT 3.5 and newer.
 */
static ares_bool_t get_REG_SZ(HKEY hKey, const char *leafKeyName, char **outptr)
{
  DWORD size = 0;
  int   res;

  *outptr = NULL;

  /* Find out size of string stored in registry */
  res = RegQueryValueExA(hKey, leafKeyName, 0, NULL, NULL, &size);
  if ((res != ERROR_SUCCESS && res != ERROR_MORE_DATA) || !size) {
    return ARES_FALSE;
  }

  /* Allocate buffer of indicated size plus one given that string
     might have been stored without null termination */
  *outptr = ares_malloc(size + 1);
  if (!*outptr) {
    return ARES_FALSE;
  }

  /* Get the value for real */
  res = RegQueryValueExA(hKey, leafKeyName, 0, NULL, (unsigned char *)*outptr,
                         &size);
  if ((res != ERROR_SUCCESS) || (size == 1)) {
    ares_free(*outptr);
    *outptr = NULL;
    return ARES_FALSE;
  }

  /* Null terminate buffer allways */
  *(*outptr + size) = '\0';

  return ARES_TRUE;
}

static void commanjoin(char **dst, const char * const src, const size_t len)
{
  char  *newbuf;
  size_t newsize;

  /* 1 for terminating 0 and 2 for , and terminating 0 */
  newsize = len + (*dst ? (ares_strlen(*dst) + 2) : 1);
  newbuf  = ares_realloc(*dst, newsize);
  if (!newbuf) {
    return;
  }
  if (*dst == NULL) {
    *newbuf = '\0';
  }
  *dst = newbuf;
  if (ares_strlen(*dst) != 0) {
    strcat(*dst, ",");
  }
  strncat(*dst, src, len);
}

/*
 * commajoin()
 *
 * RTF code.
 */
static void commajoin(char **dst, const char *src)
{
  commanjoin(dst, src, ares_strlen(src));
}

/* A structure to hold the string form of IPv4 and IPv6 addresses so we can
 * sort them by a metric.
 */
typedef struct {
  /* The metric we sort them by. */
  ULONG  metric;

  /* Original index of the item, used as a secondary sort parameter to make
   * qsort() stable if the metrics are equal */
  size_t orig_idx;

  /* Room enough for the string form of any IPv4 or IPv6 address that
   * ares_inet_ntop() will create.  Based on the existing c-ares practice.
   */
  char   text[INET6_ADDRSTRLEN + 8]; /* [%s]:NNNNN */
} Address;

/* Sort Address values \a left and \a right by metric, returning the usual
 * indicators for qsort().
 */
static int compareAddresses(const void *arg1, const void *arg2)
{
  const Address * const left  = arg1;
  const Address * const right = arg2;
  /* Lower metric the more preferred */
  if (left->metric < right->metric) {
    return -1;
  }
  if (left->metric > right->metric) {
    return 1;
  }
  /* If metrics are equal, lower original index more preferred */
  if (left->orig_idx < right->orig_idx) {
    return -1;
  }
  if (left->orig_idx > right->orig_idx) {
    return 1;
  }
  return 0;
}

/* There can be multiple routes to "the Internet".  And there can be different
 * DNS servers associated with each of the interfaces that offer those routes.
 * We have to assume that any DNS server can serve any request.  But, some DNS
 * servers may only respond if requested over their associated interface.  But
 * we also want to use "the preferred route to the Internet" whenever possible
 * (and not use DNS servers on a non-preferred route even by forcing request
 * to go out on the associated non-preferred interface).  i.e. We want to use
 * the DNS servers associated with the same interface that we would use to
 * make a general request to anything else.
 *
 * But, Windows won't sort the DNS servers by the metrics associated with the
 * routes and interfaces _even_ though it obviously sends IP packets based on
 * those same routes and metrics.  So, we must do it ourselves.
 *
 * So, we sort the DNS servers by the same metric values used to determine how
 * an outgoing IP packet will go, thus effectively using the DNS servers
 * associated with the interface that the DNS requests themselves will
 * travel.  This gives us optimal routing and avoids issues where DNS servers
 * won't respond to requests that don't arrive via some specific subnetwork
 * (and thus some specific interface).
 *
 * This function computes the metric we use to sort.  On the interface
 * identified by \a luid, it determines the best route to \a dest and combines
 * that route's metric with \a interfaceMetric to compute a metric for the
 * destination address on that interface.  This metric can be used as a weight
 * to sort the DNS server addresses associated with each interface (lower is
 * better).
 *
 * Note that by restricting the route search to the specific interface with
 * which the DNS servers are associated, this function asks the question "What
 * is the metric for sending IP packets to this DNS server?" which allows us
 * to sort the DNS servers correctly.
 */
static ULONG getBestRouteMetric(IF_LUID * const luid, /* Can't be const :( */
                                const SOCKADDR_INET * const dest,
                                const ULONG                 interfaceMetric)
{
  /* On this interface, get the best route to that destination. */
#  if defined(__WATCOMC__)
  /* OpenWatcom's builtin Windows SDK does not have a definition for
   * MIB_IPFORWARD_ROW2, and also does not allow the usage of SOCKADDR_INET
   * as a variable. Let's work around this by returning the worst possible
   * metric, but only when using the OpenWatcom compiler.
   * It may be worth investigating using a different version of the Windows
   * SDK with OpenWatcom in the future, though this may be fixed in OpenWatcom
   * 2.0.
   */
  return (ULONG)-1;
#  else
  MIB_IPFORWARD_ROW2 row;
  SOCKADDR_INET      ignored;
  if (GetBestRoute2(/* The interface to use.  The index is ignored since we are
                     * passing a LUID.
                     */
                    luid, 0,
                    /* No specific source address. */
                    NULL,
                    /* Our destination address. */
                    dest,
                    /* No options. */
                    0,
                    /* The route row. */
                    &row,
                    /* The best source address, which we don't need. */
                    &ignored) != NO_ERROR
      /* If the metric is "unused" (-1) or too large for us to add the two
       * metrics, use the worst possible, thus sorting this last.
       */
      || row.Metric == (ULONG)-1 ||
      row.Metric > ((ULONG)-1) - interfaceMetric) {
    /* Return the worst possible metric. */
    return (ULONG)-1;
  }

  /* Return the metric value from that row, plus the interface metric.
   *
   * See
   * http://msdn.microsoft.com/en-us/library/windows/desktop/aa814494(v=vs.85).aspx
   * which describes the combination as a "sum".
   */
  return row.Metric + interfaceMetric;
#  endif /* __WATCOMC__ */
}

/*
 * get_DNS_Windows()
 *
 * Locates DNS info using GetAdaptersAddresses() function from the Internet
 * Protocol Helper (IP Helper) API. When located, this returns a pointer
 * in *outptr to a newly allocated memory area holding a null-terminated
 * string with a space or comma seperated list of DNS IP addresses.
 *
 * Returns 0 and nullifies *outptr upon inability to return DNSes string.
 *
 * Returns 1 and sets *outptr when returning a dynamically allocated string.
 *
 * Implementation supports Windows XP and newer.
 */
#  define IPAA_INITIAL_BUF_SZ 15 * 1024
#  define IPAA_MAX_TRIES      3

static ares_bool_t get_DNS_Windows(char **outptr)
{
  IP_ADAPTER_DNS_SERVER_ADDRESS *ipaDNSAddr;
  IP_ADAPTER_ADDRESSES          *ipaa;
  IP_ADAPTER_ADDRESSES          *newipaa;
  IP_ADAPTER_ADDRESSES          *ipaaEntry;
  ULONG                          ReqBufsz  = IPAA_INITIAL_BUF_SZ;
  ULONG                          Bufsz     = IPAA_INITIAL_BUF_SZ;
  ULONG                          AddrFlags = 0;
  int                            trying    = IPAA_MAX_TRIES;
  int                            res;

  /* The capacity of addresses, in elements. */
  size_t                         addressesSize;
  /* The number of elements in addresses. */
  size_t                         addressesIndex = 0;
  /* The addresses we will sort. */
  Address                       *addresses;

  union {
    struct sockaddr     *sa;
    struct sockaddr_in  *sa4;
    struct sockaddr_in6 *sa6;
  } namesrvr;

  *outptr = NULL;

  ipaa = ares_malloc(Bufsz);
  if (!ipaa) {
    return ARES_FALSE;
  }

  /* Start with enough room for a few DNS server addresses and we'll grow it
   * as we encounter more.
   */
  addressesSize = 4;
  addresses     = (Address *)ares_malloc(sizeof(Address) * addressesSize);
  if (addresses == NULL) {
    /* We need room for at least some addresses to function. */
    ares_free(ipaa);
    return ARES_FALSE;
  }

  /* Usually this call suceeds with initial buffer size */
  res = GetAdaptersAddresses(AF_UNSPEC, AddrFlags, NULL, ipaa, &ReqBufsz);
  if ((res != ERROR_BUFFER_OVERFLOW) && (res != ERROR_SUCCESS)) {
    goto done;
  }

  while ((res == ERROR_BUFFER_OVERFLOW) && (--trying)) {
    if (Bufsz < ReqBufsz) {
      newipaa = ares_realloc(ipaa, ReqBufsz);
      if (!newipaa) {
        goto done;
      }
      Bufsz = ReqBufsz;
      ipaa  = newipaa;
    }
    res = GetAdaptersAddresses(AF_UNSPEC, AddrFlags, NULL, ipaa, &ReqBufsz);
    if (res == ERROR_SUCCESS) {
      break;
    }
  }
  if (res != ERROR_SUCCESS) {
    goto done;
  }

  for (ipaaEntry = ipaa; ipaaEntry; ipaaEntry = ipaaEntry->Next) {
    if (ipaaEntry->OperStatus != IfOperStatusUp) {
      continue;
    }

    /* For each interface, find any associated DNS servers as IPv4 or IPv6
     * addresses.  For each found address, find the best route to that DNS
     * server address _on_ _that_ _interface_ (at this moment in time) and
     * compute the resulting total metric, just as Windows routing will do.
     * Then, sort all the addresses found by the metric.
     */
    for (ipaDNSAddr = ipaaEntry->FirstDnsServerAddress; ipaDNSAddr;
         ipaDNSAddr = ipaDNSAddr->Next) {
      char ipaddr[INET6_ADDRSTRLEN] = "";
      namesrvr.sa                   = ipaDNSAddr->Address.lpSockaddr;

      if (namesrvr.sa->sa_family == AF_INET) {
        if ((namesrvr.sa4->sin_addr.S_un.S_addr == INADDR_ANY) ||
            (namesrvr.sa4->sin_addr.S_un.S_addr == INADDR_NONE)) {
          continue;
        }

        /* Allocate room for another address, if necessary, else skip. */
        if (addressesIndex == addressesSize) {
          const size_t    newSize = addressesSize + 4;
          Address * const newMem =
            (Address *)ares_realloc(addresses, sizeof(Address) * newSize);
          if (newMem == NULL) {
            continue;
          }
          addresses     = newMem;
          addressesSize = newSize;
        }

        addresses[addressesIndex].metric =
          getBestRouteMetric(&ipaaEntry->Luid, (SOCKADDR_INET *)(namesrvr.sa),
                             ipaaEntry->Ipv4Metric);

        /* Record insertion index to make qsort stable */
        addresses[addressesIndex].orig_idx = addressesIndex;

        if (!ares_inet_ntop(AF_INET, &namesrvr.sa4->sin_addr, ipaddr,
                            sizeof(ipaddr))) {
          continue;
        }
        snprintf(addresses[addressesIndex].text,
                 sizeof(addresses[addressesIndex].text), "[%s]:%u", ipaddr,
                 ntohs(namesrvr.sa4->sin_port));
        ++addressesIndex;
      } else if (namesrvr.sa->sa_family == AF_INET6) {
        if (memcmp(&namesrvr.sa6->sin6_addr, &ares_in6addr_any,
                   sizeof(namesrvr.sa6->sin6_addr)) == 0) {
          continue;
        }

        /* Allocate room for another address, if necessary, else skip. */
        if (addressesIndex == addressesSize) {
          const size_t    newSize = addressesSize + 4;
          Address * const newMem =
            (Address *)ares_realloc(addresses, sizeof(Address) * newSize);
          if (newMem == NULL) {
            continue;
          }
          addresses     = newMem;
          addressesSize = newSize;
        }

        addresses[addressesIndex].metric =
          getBestRouteMetric(&ipaaEntry->Luid, (SOCKADDR_INET *)(namesrvr.sa),
                             ipaaEntry->Ipv6Metric);

        /* Record insertion index to make qsort stable */
        addresses[addressesIndex].orig_idx = addressesIndex;

        if (!ares_inet_ntop(AF_INET6, &namesrvr.sa6->sin6_addr, ipaddr,
                            sizeof(ipaddr))) {
          continue;
        }
        snprintf(addresses[addressesIndex].text,
                 sizeof(addresses[addressesIndex].text), "[%s]:%u", ipaddr,
                 ntohs(namesrvr.sa6->sin6_port));
        ++addressesIndex;
      } else {
        /* Skip non-IPv4/IPv6 addresses completely. */
        continue;
      }
    }
  }

  /* Sort all of the textual addresses by their metric (and original index if
   * metrics are equal). */
  qsort(addresses, addressesIndex, sizeof(*addresses), compareAddresses);

  /* Join them all into a single string, removing duplicates. */
  {
    size_t i;
    for (i = 0; i < addressesIndex; ++i) {
      size_t j;
      /* Look for this address text appearing previously in the results. */
      for (j = 0; j < i; ++j) {
        if (strcmp(addresses[j].text, addresses[i].text) == 0) {
          break;
        }
      }
      /* Iff we didn't emit this address already, emit it now. */
      if (j == i) {
        /* Add that to outptr (if we can). */
        commajoin(outptr, addresses[i].text);
      }
    }
  }

done:
  ares_free(addresses);

  if (ipaa) {
    ares_free(ipaa);
  }

  if (!*outptr) {
    return ARES_FALSE;
  }

  return ARES_TRUE;
}

/*
 * get_SuffixList_Windows()
 *
 * Reads the "DNS Suffix Search List" from registry and writes the list items
 * whitespace separated to outptr. If the Search List is empty, the
 * "Primary Dns Suffix" is written to outptr.
 *
 * Returns 0 and nullifies *outptr upon inability to return the suffix list.
 *
 * Returns 1 and sets *outptr when returning a dynamically allocated string.
 *
 * Implementation supports Windows Server 2003 and newer
 */
static ares_bool_t get_SuffixList_Windows(char **outptr)
{
  HKEY  hKey;
  HKEY  hKeyEnum;
  char  keyName[256];
  DWORD keyNameBuffSize;
  DWORD keyIdx = 0;
  char *p      = NULL;

  *outptr = NULL;

  if (ares__getplatform() != WIN_NT) {
    return ARES_FALSE;
  }

  /* 1. Global DNS Suffix Search List */
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, WIN_NS_NT_KEY, 0, KEY_READ, &hKey) ==
      ERROR_SUCCESS) {
    get_REG_SZ(hKey, SEARCHLIST_KEY, outptr);
    if (get_REG_SZ(hKey, DOMAIN_KEY, &p)) {
      commajoin(outptr, p);
      ares_free(p);
      p = NULL;
    }
    RegCloseKey(hKey);
  }

  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, WIN_NT_DNSCLIENT, 0, KEY_READ, &hKey) ==
      ERROR_SUCCESS) {
    if (get_REG_SZ(hKey, SEARCHLIST_KEY, &p)) {
      commajoin(outptr, p);
      ares_free(p);
      p = NULL;
    }
    RegCloseKey(hKey);
  }

  /* 2. Connection Specific Search List composed of:
   *  a. Primary DNS Suffix */
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, WIN_DNSCLIENT, 0, KEY_READ, &hKey) ==
      ERROR_SUCCESS) {
    if (get_REG_SZ(hKey, PRIMARYDNSSUFFIX_KEY, &p)) {
      commajoin(outptr, p);
      ares_free(p);
      p = NULL;
    }
    RegCloseKey(hKey);
  }

  /*  b. Interface SearchList, Domain, DhcpDomain */
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, WIN_NS_NT_KEY "\\" INTERFACES_KEY, 0,
                    KEY_READ, &hKey) == ERROR_SUCCESS) {
    for (;;) {
      keyNameBuffSize = sizeof(keyName);
      if (RegEnumKeyExA(hKey, keyIdx++, keyName, &keyNameBuffSize, 0, NULL,
                        NULL, NULL) != ERROR_SUCCESS) {
        break;
      }
      if (RegOpenKeyExA(hKey, keyName, 0, KEY_QUERY_VALUE, &hKeyEnum) !=
          ERROR_SUCCESS) {
        continue;
      }
      /* p can be comma separated (SearchList) */
      if (get_REG_SZ(hKeyEnum, SEARCHLIST_KEY, &p)) {
        commajoin(outptr, p);
        ares_free(p);
        p = NULL;
      }
      if (get_REG_SZ(hKeyEnum, DOMAIN_KEY, &p)) {
        commajoin(outptr, p);
        ares_free(p);
        p = NULL;
      }
      if (get_REG_SZ(hKeyEnum, DHCPDOMAIN_KEY, &p)) {
        commajoin(outptr, p);
        ares_free(p);
        p = NULL;
      }
      RegCloseKey(hKeyEnum);
    }
    RegCloseKey(hKey);
  }

  return *outptr != NULL ? ARES_TRUE : ARES_FALSE;
}

#endif

static ares_status_t init_by_resolv_conf(ares_channel channel)
{
#if !defined(ANDROID) && !defined(__ANDROID__) && !defined(WATT32) && \
  !defined(CARES_USE_LIBRESOLV)
  char *line = NULL;
#endif
  ares_status_t        status   = ARES_EOF;
  size_t               nservers = 0;
  size_t               nsort    = 0;
  struct server_state *servers  = NULL;
  struct apattern     *sortlist = NULL;

#ifdef WIN32

  if (channel->nservers > 0) { /* don't override ARES_OPT_SERVER */
    return ARES_SUCCESS;
  }

  if (get_DNS_Windows(&line)) {
    status = config_nameserver(&servers, &nservers, line);
    ares_free(line);
  }

  if (channel->ndomains == 0 && get_SuffixList_Windows(&line)) {
    status = set_search(channel, line);
    ares_free(line);
  }

  if (status == ARES_SUCCESS) {
    status = ARES_EOF;
  } else {
    /* Catch the case when all the above checks fail (which happens when there
       is no network card or the cable is unplugged) */
    status = ARES_EFILE;
  }
#elif defined(__MVS__)

  struct __res_state  *res = 0;
  size_t               count4;
  size_t               count6;
  __STATEEXTIPV6      *v6;
  struct server_state *pserver;
  if (0 == res) {
    int rc = res_init();
    while (rc == -1 && h_errno == TRY_AGAIN) {
      rc = res_init();
    }
    if (rc == -1) {
      return ARES_ENOMEM;
    }
    res = __res();
  }

  v6 = res->__res_extIPv6;
  if (res->nscount > 0) {
    count4 = (size_t)res->nscount;
  }

  if (v6 && v6->__stat_nscount > 0) {
    count6 = (size_t)v6->__stat_nscount;
  } else {
    count6 = 0;
  }

  nservers = (size_t)(count4 + count6);
  servers  = ares_malloc(nservers * sizeof(*servers));
  if (!servers) {
    return ARES_ENOMEM;
  }

  memset(servers, 0, nservers * sizeof(*servers));

  pserver = servers;
  for (int i = 0; i < count4; ++i, ++pserver) {
    struct sockaddr_in *addr_in = &(res->nsaddr_list[i]);
    pserver->addr.addrV4.s_addr = addr_in->sin_addr.s_addr;
    pserver->addr.family        = AF_INET;
    pserver->addr.udp_port      = addr_in->sin_port;
    pserver->addr.tcp_port      = addr_in->sin_port;
  }

  for (int j = 0; j < count6; ++j, ++pserver) {
    struct sockaddr_in6 *addr_in = &(v6->__stat_nsaddr_list[j]);
    memcpy(&(pserver->addr.addr.addr6), &(addr_in->sin6_addr),
           sizeof(addr_in->sin6_addr));
    pserver->addr.family   = AF_INET6;
    pserver->addr.udp_port = addr_in->sin6_port;
    pserver->addr.tcp_port = addr_in->sin6_port;
  }

  status = ARES_EOF;

#elif defined(__riscos__)

  /* Under RISC OS, name servers are listed in the
     system variable Inet$Resolvers, space separated. */

  line   = getenv("Inet$Resolvers");
  status = ARES_EOF;
  if (line) {
    char *resolvers = ares_strdup(line), *pos, *space;

    if (!resolvers) {
      return ARES_ENOMEM;
    }

    pos = resolvers;
    do {
      space = strchr(pos, ' ');
      if (space) {
        *space = '\0';
      }
      status = config_nameserver(&servers, &nservers, pos);
      if (status != ARES_SUCCESS) {
        break;
      }
      pos = space + 1;
    } while (space);

    if (status == ARES_SUCCESS) {
      status = ARES_EOF;
    }

    ares_free(resolvers);
  }

#elif defined(WATT32)
  size_t i;

  sock_init();
  for (i = 0; def_nameservers[i]; i++)
    ;
  if (i == 0) {
    return ARES_SUCCESS; /* use localhost DNS server */
  }

  nservers = i;
  servers  = ares_malloc(nservers * sizeof(*servers));
  if (!servers) {
    return ARES_ENOMEM;
  }
  memset(servers, 0, nservers * sizeof(*servers));

  for (i = 0; def_nameservers[i]; i++) {
    servers[i].addr.addrV4.s_addr = htonl(def_nameservers[i]);
    servers[i].addr.family        = AF_INET;
    servers[i].addr.udp_port      = 0;
    servers[i].addr.tcp_port      = 0;
  }
  status = ARES_EOF;

#elif defined(ANDROID) || defined(__ANDROID__)
  size_t i;
  char **dns_servers;
  char  *domains;
  size_t num_servers;

  /* Use the Android connectivity manager to get a list
   * of DNS servers. As of Android 8 (Oreo) net.dns#
   * system properties are no longer available. Google claims this
   * improves privacy. Apps now need the ACCESS_NETWORK_STATE
   * permission and must use the ConnectivityManager which
   * is Java only. */
  dns_servers = ares_get_android_server_list(MAX_DNS_PROPERTIES, &num_servers);
  if (dns_servers != NULL) {
    for (i = 0; i < num_servers; i++) {
      status = config_nameserver(&servers, &nservers, dns_servers[i]);
      if (status != ARES_SUCCESS) {
        break;
      }
      status = ARES_EOF;
    }
    for (i = 0; i < num_servers; i++) {
      ares_free(dns_servers[i]);
    }
    ares_free(dns_servers);
  }
  if (channel->ndomains == 0) {
    domains = ares_get_android_search_domains_list();
    set_search(channel, domains);
    ares_free(domains);
  }

#  ifdef HAVE___SYSTEM_PROPERTY_GET
  /* Old way using the system property still in place as
   * a fallback. Older android versions can still use this.
   * it's possible for older apps not not have added the new
   * permission and we want to try to avoid breaking those.
   *
   * We'll only run this if we don't have any dns servers
   * because this will get the same ones (if it works). */
  if (status != ARES_EOF) {
    char propname[PROP_NAME_MAX];
    char propvalue[PROP_VALUE_MAX] = "";
    for (i = 1; i <= MAX_DNS_PROPERTIES; i++) {
      snprintf(propname, sizeof(propname), "%s%u", DNS_PROP_NAME_PREFIX, i);
      if (__system_property_get(propname, propvalue) < 1) {
        status = ARES_EOF;
        break;
      }

      status = config_nameserver(&servers, &nservers, propvalue);
      if (status != ARES_SUCCESS) {
        break;
      }
      status = ARES_EOF;
    }
  }
#  endif /* HAVE___SYSTEM_PROPERTY_GET */
#elif defined(CARES_USE_LIBRESOLV)
  struct __res_state res;
  int                result;
  memset(&res, 0, sizeof(res));
  result = res_ninit(&res);
  if (result == 0 && (res.options & RES_INIT)) {
    status = ARES_EOF;

    if (channel->nservers == 0) {
      union res_sockaddr_union addr[MAXNS];
      int                      nscount = res_getservers(&res, addr, MAXNS);
      int                      i;
      for (i = 0; i < nscount; ++i) {
        char           ipaddr[INET6_ADDRSTRLEN] = "";
        char           ipaddr_port[INET6_ADDRSTRLEN + 8]; /* [%s]:NNNNN */
        unsigned short port = 0;
        ares_status_t  config_status;
        sa_family_t    family = addr[i].sin.sin_family;
        if (family == AF_INET) {
          ares_inet_ntop(family, &addr[i].sin.sin_addr, ipaddr, sizeof(ipaddr));
          port = ntohs(addr[i].sin.sin_port);
        } else if (family == AF_INET6) {
          ares_inet_ntop(family, &addr[i].sin6.sin6_addr, ipaddr,
                         sizeof(ipaddr));
          port = ntohs(addr[i].sin6.sin6_port);
        } else {
          continue;
        }

        if (port) {
          snprintf(ipaddr_port, sizeof(ipaddr_port), "[%s]:%u", ipaddr, port);
        } else {
          snprintf(ipaddr_port, sizeof(ipaddr_port), "%s", ipaddr);
        }

        config_status = config_nameserver(&servers, &nservers, ipaddr_port);
        if (config_status != ARES_SUCCESS) {
          status = config_status;
          break;
        }
      }
    }
    if (channel->ndomains == 0) {
      size_t entries = 0;
      while ((entries < MAXDNSRCH) && res.dnsrch[entries]) {
        entries++;
      }
      if (entries) {
        channel->domains = ares_malloc(entries * sizeof(char *));
        if (!channel->domains) {
          status = ARES_ENOMEM;
        } else {
          size_t i;
          channel->ndomains = entries;
          for (i = 0; i < channel->ndomains; ++i) {
            channel->domains[i] = ares_strdup(res.dnsrch[i]);
            if (!channel->domains[i]) {
              status = ARES_ENOMEM;
            }
          }
        }
      }
    }
    if (channel->ndots == 0 && res.ndots > 0) {
      channel->ndots = (size_t)res.ndots;
    }

    if (channel->tries == 0 && res.retry > 0) {
      channel->tries = (size_t)res.retry;
    }

    if (!(channel->optmask & (ARES_OPT_ROTATE | ARES_OPT_NOROTATE))) {
      channel->rotate = (res.options & RES_ROTATE) ? ARES_TRUE : ARES_FALSE;
    }

    if (channel->timeout == 0) {
      if (res.retrans > 0) {
        channel->timeout = (unsigned int)res.retrans * 1000;
      }
#  ifdef __APPLE__
      if (res.retry >= 0) {
        channel->timeout /= ((unsigned int)res.retry + 1) *
                            (unsigned int)(res.nscount > 0 ? res.nscount : 1);
      }
#  endif
    }

    res_ndestroy(&res);
  }
#else
  {
    char       *p;
    FILE       *fp;
    size_t      linesize;
    int         error;
    int         update_domains;
    const char *resolvconf_path;

    /* Don't read resolv.conf and friends if we don't have to */
    if (ARES_CONFIG_CHECK(channel)) {
      return ARES_SUCCESS;
    }

    /* Only update search domains if they're not already specified */
    update_domains = (channel->ndomains == 0);

    /* Support path for resolvconf filename set by ares_init_options */
    if (channel->resolvconf_path) {
      resolvconf_path = channel->resolvconf_path;
    } else {
      resolvconf_path = PATH_RESOLV_CONF;
    }

    fp = fopen(resolvconf_path, "r");
    if (fp) {
      while ((status = ares__read_line(fp, &line, &linesize)) == ARES_SUCCESS) {
        if ((p = try_config(line, "domain", ';')) && update_domains) {
          status = config_domain(channel, p);
        } else if ((p = try_config(line, "lookup", ';')) && !channel->lookups) {
          status = config_lookup(channel, p, "bind", NULL, "file");
        } else if ((p = try_config(line, "search", ';')) && update_domains) {
          status = set_search(channel, p);
        } else if ((p = try_config(line, "nameserver", ';')) &&
                   channel->nservers == 0) {
          status = config_nameserver(&servers, &nservers, p);
        } else if ((p = try_config(line, "sortlist", ';')) &&
                   !(channel->optmask & ARES_OPT_SORTLIST)) {
          status = config_sortlist(&sortlist, &nsort, p);
        } else if ((p = try_config(line, "options", ';'))) {
          status = set_options(channel, p);
        } else {
          status = ARES_SUCCESS;
        }
        if (status != ARES_SUCCESS) {
          break;
        }
      }
      fclose(fp);
    } else {
      error = ERRNO;
      switch (error) {
        case ENOENT:
        case ESRCH:
          status = ARES_EOF;
          break;
        default:
          DEBUGF(fprintf(stderr, "fopen() failed with error: %d %s\n", error,
                         strerror(error)));
          DEBUGF(fprintf(stderr, "Error opening file: %s\n", PATH_RESOLV_CONF));
          status = ARES_EFILE;
      }
    }

    if ((status == ARES_EOF) && (!channel->lookups)) {
      /* Many systems (Solaris, Linux, BSD's) use nsswitch.conf */
      fp = fopen("/etc/nsswitch.conf", "r");
      if (fp) {
        while ((status = ares__read_line(fp, &line, &linesize)) ==
               ARES_SUCCESS) {
          if ((p = try_config(line, "hosts:", '\0')) && !channel->lookups) {
            (void)config_lookup(channel, p, "dns", "resolve", "files");
          }
        }
        fclose(fp);
      } else {
        error = ERRNO;
        switch (error) {
          case ENOENT:
          case ESRCH:
            break;
          default:
            DEBUGF(fprintf(stderr, "fopen() failed with error: %d %s\n", error,
                           strerror(error)));
            DEBUGF(fprintf(stderr, "Error opening file: %s\n",
                           "/etc/nsswitch.conf"));
        }

        /* ignore error, maybe we will get luck in next if clause */
        status = ARES_EOF;
      }
    }

    if ((status == ARES_EOF) && (!channel->lookups)) {
      /* Linux / GNU libc 2.x and possibly others have host.conf */
      fp = fopen("/etc/host.conf", "r");
      if (fp) {
        while ((status = ares__read_line(fp, &line, &linesize)) ==
               ARES_SUCCESS) {
          if ((p = try_config(line, "order", '\0')) && !channel->lookups) {
            /* ignore errors */
            (void)config_lookup(channel, p, "bind", NULL, "hosts");
          }
        }
        fclose(fp);
      } else {
        error = ERRNO;
        switch (error) {
          case ENOENT:
          case ESRCH:
            break;
          default:
            DEBUGF(fprintf(stderr, "fopen() failed with error: %d %s\n", error,
                           strerror(error)));
            DEBUGF(
              fprintf(stderr, "Error opening file: %s\n", "/etc/host.conf"));
        }

        /* ignore error, maybe we will get luck in next if clause */
        status = ARES_EOF;
      }
    }

    if ((status == ARES_EOF) && (!channel->lookups)) {
      /* Tru64 uses /etc/svc.conf */
      fp = fopen("/etc/svc.conf", "r");
      if (fp) {
        while ((status = ares__read_line(fp, &line, &linesize)) ==
               ARES_SUCCESS) {
          if ((p = try_config(line, "hosts=", '\0')) && !channel->lookups) {
            /* ignore errors */
            (void)config_lookup(channel, p, "bind", NULL, "local");
          }
        }
        fclose(fp);
      } else {
        error = ERRNO;
        switch (error) {
          case ENOENT:
          case ESRCH:
            break;
          default:
            DEBUGF(fprintf(stderr, "fopen() failed with error: %d %s\n", error,
                           strerror(error)));
            DEBUGF(
              fprintf(stderr, "Error opening file: %s\n", "/etc/svc.conf"));
        }

        /* ignore error, default value will be chosen for `channel->lookups` */
        status = ARES_EOF;
      }
    }

    if (line) {
      ares_free(line);
    }
  }

#endif

  /* Handle errors. */
  if (status != ARES_EOF) {
    if (servers != NULL) {
      ares_free(servers);
    }
    if (sortlist != NULL) {
      ares_free(sortlist);
    }
    return status;
  }

  /* If we got any name server entries, fill them in. */
  if (servers) {
    channel->servers  = servers;
    channel->nservers = nservers;
  }

  /* If we got any sortlist entries, fill them in. */
  if (sortlist) {
    channel->sortlist = sortlist;
    channel->nsort    = nsort;
  }

  return ARES_SUCCESS;
}

static ares_status_t init_by_defaults(ares_channel channel)
{
  char         *hostname = NULL;
  ares_status_t rc       = ARES_SUCCESS;
#ifdef HAVE_GETHOSTNAME
  char *dot;
#endif

  if (channel->timeout == 0) {
    channel->timeout = DEFAULT_TIMEOUT;
  }

  if (channel->tries == 0) {
    channel->tries = DEFAULT_TRIES;
  }

  if (channel->ndots == 0) {
    channel->ndots = 1;
  }

  if (channel->udp_port == 0) {
    channel->udp_port = htons(NAMESERVER_PORT);
  }
  if (channel->tcp_port == 0) {
    channel->tcp_port = htons(NAMESERVER_PORT);
  }

  if (channel->ednspsz == 0) {
    channel->ednspsz = EDNSPACKETSZ;
  }

  if (channel->nservers == 0) {
    /* If nobody specified servers, try a local named. */
    channel->servers = ares_malloc(sizeof(*channel->servers));
    if (!channel->servers) {
      rc = ARES_ENOMEM;
      goto error;
    }
    memset(channel->servers, 0, sizeof(*channel->servers));
    channel->servers[0].addr.family        = AF_INET;
    channel->servers[0].addr.addrV4.s_addr = htonl(INADDR_LOOPBACK);
    channel->servers[0].addr.udp_port      = 0;
    channel->servers[0].addr.tcp_port      = 0;
    channel->nservers                      = 1;
  }

#if defined(USE_WINSOCK)
#  define toolong(x) (x == -1) && (SOCKERRNO == WSAEFAULT)
#elif defined(ENAMETOOLONG)
#  define toolong(x) \
    (x == -1) && ((SOCKERRNO == ENAMETOOLONG) || (SOCKERRNO == EINVAL))
#else
#  define toolong(x) (x == -1) && (SOCKERRNO == EINVAL)
#endif

  if (channel->ndomains == 0) {
    /* Derive a default domain search list from the kernel hostname,
     * or set it to empty if the hostname isn't helpful.
     */
#ifndef HAVE_GETHOSTNAME
    channel->ndomains = 0; /* default to none */
#else
    GETHOSTNAME_TYPE_ARG2 lenv = 64;
    size_t                len  = 64;
    int                   res;
    channel->ndomains = 0; /* default to none */

    hostname = ares_malloc(len);
    if (!hostname) {
      rc = ARES_ENOMEM;
      goto error;
    }

    do {
      res = gethostname(hostname, lenv);

      if (toolong(res)) {
        char *p;
        len  *= 2;
        lenv *= 2;
        p     = ares_realloc(hostname, len);
        if (!p) {
          rc = ARES_ENOMEM;
          goto error;
        }
        hostname = p;
        continue;
      } else if (res) {
        /* Lets not treat a gethostname failure as critical, since we
         * are ok if gethostname doesn't even exist */
        *hostname = '\0';
        break;
      }

    } while (res != 0);

    dot = strchr(hostname, '.');
    if (dot) {
      /* a dot was found */
      channel->domains = ares_malloc(sizeof(char *));
      if (!channel->domains) {
        rc = ARES_ENOMEM;
        goto error;
      }
      channel->domains[0] = ares_strdup(dot + 1);
      if (!channel->domains[0]) {
        rc = ARES_ENOMEM;
        goto error;
      }
      channel->ndomains = 1;
    }
#endif
  }

  if (channel->nsort == 0) {
    channel->sortlist = NULL;
  }

  if (!channel->lookups) {
    channel->lookups = ares_strdup("fb");
    if (!channel->lookups) {
      rc = ARES_ENOMEM;
    }
  }

error:
  if (rc) {
    if (channel->servers) {
      ares_free(channel->servers);
      channel->servers = NULL;
    }
    channel->nservers = 0;

    if (channel->domains && channel->domains[0]) {
      ares_free(channel->domains[0]);
    }
    if (channel->domains) {
      ares_free(channel->domains);
      channel->domains = NULL;
    }

    if (channel->lookups) {
      ares_free(channel->lookups);
      channel->lookups = NULL;
    }

    if (channel->resolvconf_path) {
      ares_free(channel->resolvconf_path);
      channel->resolvconf_path = NULL;
    }

    if (channel->hosts_path) {
      ares_free(channel->hosts_path);
      channel->hosts_path = NULL;
    }
  }

  if (hostname) {
    ares_free(hostname);
  }

  return rc;
}

#if !defined(WIN32) && !defined(WATT32) && !defined(ANDROID) && \
  !defined(__ANDROID__) && !defined(CARES_USE_LIBRESOLV)
static ares_status_t config_domain(ares_channel channel, char *str)
{
  char *q;

  /* Set a single search domain. */
  q = str;
  while (*q && !ISSPACE(*q)) {
    q++;
  }
  *q = '\0';
  return set_search(channel, str);
}

#  if defined(__INTEL_COMPILER) && (__INTEL_COMPILER == 910) && \
    defined(__OPTIMIZE__) && defined(__unix__) && defined(__i386__)
/* workaround icc 9.1 optimizer issue */
#    define vqualifier volatile
#  else
#    define vqualifier
#  endif

static ares_status_t config_lookup(ares_channel channel, const char *str,
                                   const char *bindch, const char *altbindch,
                                   const char *filech)
{
  char                   lookups[3];
  char                  *l;
  const char *vqualifier p;
  ares_bool_t            found;

  if (altbindch == NULL) {
    altbindch = bindch;
  }

  /* Set the lookup order.  Only the first letter of each work
   * is relevant, and it has to be "b" for DNS or "f" for the
   * host file.  Ignore everything else.
   */
  l     = lookups;
  p     = str;
  found = ARES_FALSE;
  while (*p) {
    if ((*p == *bindch || *p == *altbindch || *p == *filech) &&
        l < lookups + 2) {
      if (*p == *bindch || *p == *altbindch) {
        *l++ = 'b';
      } else {
        *l++ = 'f';
      }
      found = ARES_TRUE;
    }
    while (*p && !ISSPACE(*p) && (*p != ',')) {
      p++;
    }
    while (*p && (ISSPACE(*p) || (*p == ','))) {
      p++;
    }
  }
  if (!found) {
    return ARES_ENOTINITIALIZED;
  }
  *l               = '\0';
  channel->lookups = ares_strdup(lookups);
  return (channel->lookups) ? ARES_SUCCESS : ARES_ENOMEM;
}
#endif /* !WIN32 & !WATT32 & !ANDROID & !__ANDROID__ & !CARES_USE_LIBRESOLV */

#ifndef WATT32
/* Validate that the ip address matches the subnet (network base and network
 * mask) specified. Addresses are specified in standard Network Byte Order as
 * 16 bytes, and the netmask is 0 to 128 (bits).
 */
static ares_bool_t ares_ipv6_subnet_matches(const unsigned char netbase[16],
                                            unsigned char       netmask,
                                            const unsigned char ipaddr[16])
{
  unsigned char mask[16] = { 0 };
  unsigned char i;

  /* Misuse */
  if (netmask > 128) {
    return ARES_FALSE;
  }

  /* Quickly set whole bytes */
  memset(mask, 0xFF, netmask / 8);

  /* Set remaining bits */
  if (netmask % 8) {
    mask[netmask / 8] = (unsigned char)(0xff << (8 - (netmask % 8)));
  }

  for (i = 0; i < 16; i++) {
    if ((netbase[i] & mask[i]) != (ipaddr[i] & mask[i])) {
      return ARES_FALSE;
    }
  }

  return ARES_TRUE;
}

/* Return true iff the IPv6 ipaddr is blacklisted. */
static ares_bool_t ares_ipv6_server_blacklisted(const unsigned char ipaddr[16])
{
  /* A list of blacklisted IPv6 subnets. */
  const struct {
    const unsigned char netbase[16];
    unsigned char       netmask;
  } blacklist[] = {
  /* fec0::/10 was deprecated by [RFC3879] in September 2004. Formerly a
  * Site-Local scoped address prefix.  These are never valid DNS servers,
  * but are known to be returned at least sometimes on Windows and Android.
  */
    {{ 0xfe, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00 },
     10}
  };

  size_t i;

  /* See if ipaddr matches any of the entries in the blacklist. */
  for (i = 0; i < sizeof(blacklist) / sizeof(blacklist[0]); ++i) {
    if (ares_ipv6_subnet_matches(blacklist[i].netbase, blacklist[i].netmask,
                                 ipaddr)) {
      return ARES_TRUE;
    }
  }
  return ARES_FALSE;
}

/* Parse address and port in these formats, either ipv4 or ipv6 addresses
 * are allowed:
 *   ipaddr
 *   [ipaddr]
 *   [ipaddr]:port
 *
 * If a port is not specified, will set port to 0.
 *
 * Will fail if an IPv6 nameserver as detected by
 * ares_ipv6_server_blacklisted()
 *
 * Returns an error code on failure, else ARES_SUCCESS
 */
static ares_status_t parse_dnsaddrport(const char *str, size_t len,
                                       struct ares_addr *host,
                                       unsigned short   *port)
{
  char        ipaddr[INET6_ADDRSTRLEN] = "";
  char        ipport[6]                = "";
  size_t      mylen;
  const char *addr_start = NULL;
  const char *addr_end   = NULL;
  const char *port_start = NULL;
  const char *port_end   = NULL;

  /* Must start with [, hex digit or : */
  if (len == 0 || (*str != '[' && !isxdigit(*str) && *str != ':')) {
    return ARES_EBADSTR;
  }

  /* If it starts with a bracket, must end with a bracket */
  if (*str == '[') {
    const char *ptr;
    addr_start = str + 1;
    ptr        = memchr(addr_start, ']', len - 1);
    if (ptr == NULL) {
      return ARES_EBADSTR;
    }
    addr_end = ptr - 1;

    /* Try to pull off port */
    if ((size_t)(ptr - str) < len) {
      ptr++;
      if (*ptr != ':') {
        return ARES_EBADSTR;
      }

      /* Missing port number */
      if ((size_t)(ptr - str) == len) {
        return ARES_EBADSTR;
      }

      port_start = ptr + 1;
      port_end   = str + (len - 1);
    }
  } else {
    addr_start = str;
    addr_end   = str + (len - 1);
  }

  mylen = (size_t)(addr_end - addr_start) + 1;
  /* Larger than buffer with null term */
  if (mylen + 1 > sizeof(ipaddr)) {
    return ARES_EBADSTR;
  }

  memset(ipaddr, 0, sizeof(ipaddr));
  memcpy(ipaddr, addr_start, mylen);

  if (port_start) {
    mylen = (size_t)(port_end - port_start) + 1;
    /* Larger than buffer with null term */
    if (mylen + 1 > sizeof(ipport)) {
      return ARES_EBADSTR;
    }
    memset(ipport, 0, sizeof(ipport));
    memcpy(ipport, port_start, mylen);
  } else {
    snprintf(ipport, sizeof(ipport), "0");
  }

  /* Convert textual address to binary format. */
  if (ares_inet_pton(AF_INET, ipaddr, &host->addrV4) == 1) {
    host->family = AF_INET;
  } else if (ares_inet_pton(AF_INET6, ipaddr, &host->addrV6) == 1
             /* Silently skip blacklisted IPv6 servers. */
             && !ares_ipv6_server_blacklisted(
                  (const unsigned char *)&host->addrV6)) {
    host->family = AF_INET6;
  } else {
    return ARES_EBADSTR;
  }

  *port = (unsigned short)atoi(ipport);
  return ARES_SUCCESS;
}

/* Add the IPv4 or IPv6 nameservers in str (separated by commas or spaces) to
 * the servers list, updating servers and nservers as required.
 *
 * If a nameserver is encapsulated in [ ] it may optionally include a port
 * suffix, e.g.:
 *    [127.0.0.1]:59591
 *
 * The extended format is required to support OpenBSD's resolv.conf format:
 *   https://man.openbsd.org/OpenBSD-5.1/resolv.conf.5
 * As well as MacOS libresolv that may include a non-default port number.
 *
 * This will silently ignore blacklisted IPv6 nameservers as detected by
 * ares_ipv6_server_blacklisted().
 *
 * Returns an error code on failure, else ARES_SUCCESS.
 */
static ares_status_t config_nameserver(struct server_state **servers,
                                       size_t *nservers, const char *str)
{
  struct ares_addr     host;
  struct server_state *newserv;
  const char          *p;
  const char          *txtaddr;
  /* On Windows, there may be more than one nameserver specified in the same
   * registry key, so we parse input as a space or comma seperated list.
   */
  for (p = str; p;) {
    unsigned short port;

    /* Skip whitespace and commas. */
    while (*p && (ISSPACE(*p) || (*p == ','))) {
      p++;
    }
    if (!*p) {
      /* No more input, done. */
      break;
    }

    /* Pointer to start of IPv4 or IPv6 address part. */
    txtaddr = p;

    /* Advance past this address. */
    while (*p && !ISSPACE(*p) && (*p != ',')) {
      p++;
    }

    if (parse_dnsaddrport(txtaddr, (size_t)(p - txtaddr), &host, &port) !=
        ARES_SUCCESS) {
      continue;
    }

    /* Resize servers state array. */
    newserv = ares_realloc(*servers, (*nservers + 1) * sizeof(*newserv));
    if (!newserv) {
      return ARES_ENOMEM;
    }

    memset(((unsigned char *)newserv) + ((*nservers) * sizeof(*newserv)), 0,
           sizeof(*newserv));

    /* Store address data. */
    newserv[*nservers].addr.family   = host.family;
    newserv[*nservers].addr.udp_port = htons(port);
    newserv[*nservers].addr.tcp_port = htons(port);
    if (host.family == AF_INET) {
      memcpy(&newserv[*nservers].addr.addrV4, &host.addrV4,
             sizeof(host.addrV4));
    } else {
      memcpy(&newserv[*nservers].addr.addrV6, &host.addrV6,
             sizeof(host.addrV6));
    }

    /* Update arguments. */
    *servers   = newserv;
    *nservers += 1;
  }

  return ARES_SUCCESS;
}
#endif /* !WATT32 */

static ares_status_t config_sortlist(struct apattern **sortlist, size_t *nsort,
                                     const char *str)
{
  struct apattern pat;
  const char     *q;

  *sortlist = NULL;
  *nsort    = 0;

  /* Add sortlist entries. */
  while (*str && *str != ';') {
    int    bits;
    char   ipbuf[17];
    char   ipbufpfx[32];
    size_t len;

    /* Find just the IP */
    q = str;
    while (*q && *q != '/' && *q != ';' && !ISSPACE(*q)) {
      q++;
    }

    len = (size_t)(q - str);
    if (len >= sizeof(ipbuf) - 1) {
      ares_free(*sortlist);
      *sortlist = NULL;
      return ARES_EBADSTR;
    }
    memcpy(ipbuf, str, len);
    ipbuf[len] = '\0';

    /* Find the prefix */
    if (*q == '/') {
      const char *str2 = q + 1;
      while (*q && *q != ';' && !ISSPACE(*q)) {
        q++;
      }
      if (q - str >= 32) {
        ares_free(*sortlist);
        *sortlist = NULL;
        return ARES_EBADSTR;
      }
      memcpy(ipbufpfx, str, (size_t)(q - str));
      ipbufpfx[q - str] = '\0';
      str               = str2;
    } else {
      ipbufpfx[0] = '\0';
    }
    /* Lets see if it is CIDR */
    /* First we'll try IPv6 */
    if ((bits = ares_inet_net_pton(AF_INET6, ipbufpfx[0] ? ipbufpfx : ipbuf,
                                   &pat.addrV6, sizeof(pat.addrV6))) > 0) {
      pat.type      = PATTERN_CIDR;
      pat.mask.bits = (unsigned short)bits;
      pat.family    = AF_INET6;
      if (!sortlist_alloc(sortlist, nsort, &pat)) {
        ares_free(*sortlist);
        *sortlist = NULL;
        return ARES_ENOMEM;
      }
    } else if (ipbufpfx[0] &&
               (bits = ares_inet_net_pton(AF_INET, ipbufpfx, &pat.addrV4,
                                          sizeof(pat.addrV4))) > 0) {
      pat.type      = PATTERN_CIDR;
      pat.mask.bits = (unsigned short)bits;
      pat.family    = AF_INET;
      if (!sortlist_alloc(sortlist, nsort, &pat)) {
        ares_free(*sortlist);
        *sortlist = NULL;
        return ARES_ENOMEM;
      }
    }
    /* See if it is just a regular IP */
    else if (ip_addr(ipbuf, q - str, &pat.addrV4) == 0) {
      if (ipbufpfx[0]) {
        len = (size_t)(q - str);
        if (len >= sizeof(ipbuf) - 1) {
          ares_free(*sortlist);
          *sortlist = NULL;
          return ARES_EBADSTR;
        }
        memcpy(ipbuf, str, len);
        ipbuf[len] = '\0';

        if (ip_addr(ipbuf, q - str, &pat.mask.addr4) != 0) {
          natural_mask(&pat);
        }
      } else {
        natural_mask(&pat);
      }
      pat.family = AF_INET;
      pat.type   = PATTERN_MASK;
      if (!sortlist_alloc(sortlist, nsort, &pat)) {
        ares_free(*sortlist);
        *sortlist = NULL;
        return ARES_ENOMEM;
      }
    } else {
      while (*q && *q != ';' && !ISSPACE(*q)) {
        q++;
      }
    }
    str = q;
    while (ISSPACE(*str)) {
      str++;
    }
  }

  return ARES_SUCCESS;
}

static ares_status_t set_search(ares_channel channel, const char *str)
{
  size_t cnt;

  if (channel->ndomains > 0) {
    /* LCOV_EXCL_START: all callers check ndomains == -1 */
    /* if we already have some domains present, free them first */
    ares__strsplit_free(channel->domains, (size_t)channel->ndomains);
    channel->domains  = NULL;
    channel->ndomains = 0;
  } /* LCOV_EXCL_STOP */

  channel->domains  = ares__strsplit(str, ", ", &cnt);
  channel->ndomains = cnt;
  if (channel->domains == NULL || channel->ndomains == 0) {
    channel->domains  = NULL;
    channel->ndomains = 0;
  }

  return ARES_SUCCESS;
}

static ares_status_t set_options(ares_channel channel, const char *str)
{
  const char *p;
  const char *q;
  const char *val;

  p = str;
  while (*p) {
    q = p;
    while (*q && !ISSPACE(*q)) {
      q++;
    }
    val = try_option(p, q, "ndots:");
    if (val && channel->ndots == 0) {
      channel->ndots = strtoul(val, NULL, 10);
    }

    val = try_option(p, q, "retrans:");
    if (val && channel->timeout == 0) {
      channel->timeout = strtoul(val, NULL, 10);
    }

    val = try_option(p, q, "retry:");
    if (val && channel->tries == 0) {
      channel->tries = strtoul(val, NULL, 10);
    }

    val = try_option(p, q, "rotate");
    if (val && !(channel->optmask & (ARES_OPT_ROTATE | ARES_OPT_NOROTATE))) {
      channel->rotate = ARES_TRUE;
    }

    p = q;
    while (ISSPACE(*p)) {
      p++;
    }
  }

  return ARES_SUCCESS;
}

static const char *try_option(const char *p, const char *q, const char *opt)
{
  size_t len = ares_strlen(opt);
  return ((size_t)(q - p) >= len && !strncmp(p, opt, len)) ? &p[len] : NULL;
}

#if !defined(WIN32) && !defined(WATT32) && !defined(ANDROID) && \
  !defined(__ANDROID__) && !defined(CARES_USE_LIBRESOLV)
static char *try_config(char *s, const char *opt, char scc)
{
  size_t len;
  char  *p;
  char  *q;

  if (!s || !opt) {
    /* no line or no option */
    return NULL; /* LCOV_EXCL_LINE */
  }

  /* Hash '#' character is always used as primary comment char, additionally
     a not-NUL secondary comment char will be considered when specified. */

  /* trim line comment */
  p = s;
  if (scc) {
    while (*p && (*p != '#') && (*p != scc)) {
      p++;
    }
  } else {
    while (*p && (*p != '#')) {
      p++;
    }
  }
  *p = '\0';

  /* trim trailing whitespace */
  q = p - 1;
  while ((q >= s) && ISSPACE(*q)) {
    q--;
  }
  *++q = '\0';

  /* skip leading whitespace */
  p = s;
  while (*p && ISSPACE(*p)) {
    p++;
  }

  if (!*p) {
    /* empty line */
    return NULL;
  }

  if ((len = ares_strlen(opt)) == 0) {
    /* empty option */
    return NULL; /* LCOV_EXCL_LINE */
  }

  if (strncmp(p, opt, len) != 0) {
    /* line and option do not match */
    return NULL;
  }

  /* skip over given option name */
  p += len;

  if (!*p) {
    /* no option value */
    return NULL; /* LCOV_EXCL_LINE */
  }

  if ((opt[len - 1] != ':') && (opt[len - 1] != '=') && !ISSPACE(*p)) {
    /* whitespace between option name and value is mandatory
       for given option names which do not end with ':' or '=' */
    return NULL;
  }

  /* skip over whitespace */
  while (*p && ISSPACE(*p)) {
    p++;
  }

  if (!*p) {
    /* no option value */
    return NULL;
  }

  /* return pointer to option value */
  return p;
}
#endif /* !WIN32 & !WATT32 & !ANDROID & !__ANDROID__ */

static int ip_addr(const char *ipbuf, ares_ssize_t len, struct in_addr *addr)
{
  /* Four octets and three periods yields at most 15 characters. */
  if (len > 15) {
    return -1;
  }

  if (ares_inet_pton(AF_INET, ipbuf, addr) < 1) {
    return -1;
  }

  return 0;
}

static void natural_mask(struct apattern *pat)
{
  struct in_addr addr;

  /* Store a host-byte-order copy of pat in a struct in_addr.  Icky,
   * but portable.
   */
  addr.s_addr = ntohl(pat->addrV4.s_addr);

  /* This is out of date in the CIDR world, but some people might
   * still rely on it.
   */
  if (IN_CLASSA(addr.s_addr)) {
    pat->mask.addr4.s_addr = htonl(IN_CLASSA_NET);
  } else if (IN_CLASSB(addr.s_addr)) {
    pat->mask.addr4.s_addr = htonl(IN_CLASSB_NET);
  } else {
    pat->mask.addr4.s_addr = htonl(IN_CLASSC_NET);
  }
}

static ares_bool_t sortlist_alloc(struct apattern **sortlist, size_t *nsort,
                                  struct apattern *pat)
{
  struct apattern *newsort;
  newsort = ares_realloc(*sortlist, (*nsort + 1) * sizeof(struct apattern));
  if (!newsort) {
    return ARES_FALSE;
  }
  newsort[*nsort] = *pat;
  *sortlist       = newsort;
  (*nsort)++;
  return ARES_TRUE;
}

void ares_set_local_ip4(ares_channel channel, unsigned int local_ip)
{
  channel->local_ip4 = local_ip;
}

/* local_ip6 should be 16 bytes in length */
void ares_set_local_ip6(ares_channel channel, const unsigned char *local_ip6)
{
  memcpy(&channel->local_ip6, local_ip6, sizeof(channel->local_ip6));
}

/* local_dev_name should be null terminated. */
void ares_set_local_dev(ares_channel channel, const char *local_dev_name)
{
  ares_strcpy(channel->local_dev_name, local_dev_name,
              sizeof(channel->local_dev_name));
  channel->local_dev_name[sizeof(channel->local_dev_name) - 1] = 0;
}


int ares_set_sortlist(ares_channel channel, const char *sortstr)
{
  size_t           nsort    = 0;
  struct apattern *sortlist = NULL;
  ares_status_t    status;

  if (!channel) {
    return ARES_ENODATA;
  }

  status = config_sortlist(&sortlist, &nsort, sortstr);
  if (status == ARES_SUCCESS && sortlist) {
    if (channel->sortlist) {
      ares_free(channel->sortlist);
    }
    channel->sortlist = sortlist;
    channel->nsort    = nsort;
  }
  return (int)status;
}

ares_status_t ares__init_servers_state(ares_channel channel)
{
  struct server_state *server;
  size_t               i;

  for (i = 0; i < channel->nservers; i++) {
    server = &channel->servers[i];

    /* NOTE: Can't use memset() here because the server addresses have been
     *       filled in already */
    server->tcp_parser = ares__buf_create();
    if (server->tcp_parser == NULL) {
      return ARES_ENOMEM;
    }

    server->tcp_send = ares__buf_create();
    if (server->tcp_send == NULL) {
      ares__buf_destroy(server->tcp_parser);
      return ARES_ENOMEM;
    }

    server->idx         = (size_t)i;
    server->connections = ares__llist_create(NULL);
    if (server->connections == NULL) {
      ares__buf_destroy(server->tcp_parser);
      ares__buf_destroy(server->tcp_send);
      return ARES_ENOMEM;
    }

    server->tcp_connection_generation = ++channel->tcp_connection_generation;
    server->channel                   = channel;
  }
  return ARES_SUCCESS;
}
