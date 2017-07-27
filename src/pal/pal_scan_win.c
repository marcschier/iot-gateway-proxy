// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include "os.h"
#include "util_mem.h"
#include "prx_sched.h"
#include "pal_scan.h"
#include "pal_net.h"
#include "pal_types.h"
#include "pal_err.h"
#include "util_string.h"
#include "util_misc.h"

//
// Scan task represents an individual probe
//
typedef struct pal_scan_probe pal_scan_probe_t;

//
// Tristate of scan probe
//
typedef enum pal_scan_probe_state
{
    pal_scan_probe_idle = 1,
    pal_scan_probe_working,
    pal_scan_probe_done
}
pal_scan_probe_state_t;

//
// Scan task represents an individual probe
//
struct pal_scan_probe
{
    OVERLAPPED ov;         // Must be first to cast from OVERLAPPED*
    pal_scan_t* scan;
    pal_scan_probe_state_t state;
    bool timeout;
    SOCKADDR_INET from;
    int itf_index;
    SOCKADDR_INET to;
    SOCKET socket;
};

#define MAX_PROBES 1024
#define PROBE_TIMEOUT 1000

//
// The context for ip and port scanning
//
struct pal_scan
{
    prx_scheduler_t* scheduler;
    int32_t flags;

    SOCKADDR_INET address;     // Address of host when port scanning
    uint16_t port;  // Port or 0 if only addresses are to be scanned

    pal_scan_cb_t cb;
    void* context;
    bool closed;                     // Whether the scan is complete

    uint32_t ip_scan_itf;
    uint32_t ip_scan_cur;        // next ip address or port to probe
    uint32_t ip_scan_end;         // End port or ip address to probe

    pal_scan_probe_t tasks[MAX_PROBES];            // Probe contexts
    PMIB_IPNET_TABLE2 neighbors;         // Originally returned head
    PIP_ADAPTER_ADDRESSES ifaddr;         // Allocated adapter infos

    size_t neighbors_index;           // First run through neighbors
    IP_ADAPTER_UNICAST_ADDRESS *uacur;
    PIP_ADAPTER_ADDRESSES ifcur;    // Then iterate through adapters
    log_t log;
};

static prx_scheduler_t* _scheduler;

//
// Creates, binds, and connects a socket - defined in pal_sk_win
//
extern int32_t pal_socket_create_bind_and_connect_async(
    int af,
    const struct sockaddr* from,
    int from_len,
    const struct sockaddr* to,
    int to_len,
    LPOVERLAPPED ov,
    LPOVERLAPPED_COMPLETION_ROUTINE completion,
    SOCKET* out
);

//
// Dummy callback - ensure callbacks go nowhere after close.
//
static void pal_scan_dummy_cb(
    void *context,
    uint64_t itf_index,
    int32_t error,
    prx_socket_address_t *addr
)
{
    (void)context;
    (void)itf_index;
    (void)error;
    (void)addr;
}

//
// Schedule next set of probe task for tasks that have completed
//
static void pal_scan_next(
    pal_scan_t* scan
);

//
// Clear task
//
static void pal_scan_probe_cancel(
    pal_scan_probe_t* task
)
{
    if (task->socket != _invalid_fd)
    {
        while (!HasOverlappedIoCompleted(&task->ov))
            CancelIoEx((HANDLE)task->socket, &task->ov);
        closesocket(task->socket);
        memset(&task->ov, 0, sizeof(OVERLAPPED));
        task->socket = _invalid_fd;
    }

    task->state = pal_scan_probe_idle;

    if (task->scan->scheduler)
        prx_scheduler_clear(task->scan->scheduler, NULL, task);
}

//
// Complete the task
//
static void pal_scan_probe_complete(
    pal_scan_probe_t* task
)
{
    int32_t result;
    bool found;
    prx_socket_address_t prx_addr;
    dbg_assert_ptr(task->scan);
    dbg_assert_is_task(task->scan->scheduler);

    result = pal_os_to_prx_socket_address(
        (const struct sockaddr*)&task->to, task->to.si_family == AF_INET6 ?
        sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in), &prx_addr);
    if (result != er_ok)
    {
        log_error(task->scan->log, "Failed to convert address (%s)",
            prx_err_string(result));
    }
    else
    {
        found = task->state == pal_scan_probe_done;
        if (prx_addr.un.family == prx_address_family_inet6)
        {
            log_debug(task->scan->log, "%s: [%x:%x:%x:%x:%x:%x:%x:%x]:%d",
                found ? "Found" : "Failed",
                prx_addr.un.ip.un.in6.un.u16[0], prx_addr.un.ip.un.in6.un.u16[1],
                prx_addr.un.ip.un.in6.un.u16[2], prx_addr.un.ip.un.in6.un.u16[3],
                prx_addr.un.ip.un.in6.un.u16[4], prx_addr.un.ip.un.in6.un.u16[5],
                prx_addr.un.ip.un.in6.un.u16[6], prx_addr.un.ip.un.in6.un.u16[7],
                prx_addr.un.ip.port);
        }
        else
        {
            dbg_assert(prx_addr.un.family == prx_address_family_inet, "af wrong");
            log_debug(task->scan->log, "%s: %d.%d.%d.%d:%d",
                found ? "Found" : "Failed",
                prx_addr.un.ip.un.in4.un.u8[0], prx_addr.un.ip.un.in4.un.u8[1],
                prx_addr.un.ip.un.in4.un.u8[2], prx_addr.un.ip.un.in4.un.u8[3],
                prx_addr.un.ip.port);
        }
        if (found)
        {
            task->scan->cb(task->scan->context, task->itf_index, er_ok, &prx_addr);
        }
    }

    pal_scan_probe_cancel(task);
    __do_next(task->scan, pal_scan_next);
}

//
// Io completion port operation callback when operation completed
//
static void CALLBACK pal_scan_result_from_OVERLAPPED(
    DWORD error,
    DWORD bytes,
    LPOVERLAPPED ov
)
{
    pal_scan_probe_t* task;
    (void)bytes;
    dbg_assert_ptr(ov);
    task = (pal_scan_probe_t*)ov;
    dbg_assert_ptr(task);
    dbg_assert_ptr(task->scan);
    if (task->scan->closed)
        return;
    if (error == 0)
        task->state = pal_scan_probe_done;
    __do_next_s(task->scan->scheduler, pal_scan_probe_complete, task);
}

//
// Send arp
//
static DWORD WINAPI pal_scan_probe_with_arp(
    void* context
)
{
    DWORD error;
    ULONG mac_addr[2];
    ULONG mac_addr_size = sizeof(mac_addr);
    pal_scan_probe_t* task;
    dbg_assert_ptr(context);

    task = (pal_scan_probe_t*)context;
    error = SendARP(task->to.Ipv4.sin_addr.s_addr, task->from.Ipv4.sin_addr.s_addr,
        mac_addr, &mac_addr_size);
    if (error == 0)
        task->state = pal_scan_probe_done;
    __do_next_s(task->scan->scheduler, pal_scan_probe_complete, task);
    return 0;
}

//
// Timeout performing task
//
static void pal_scan_probe_timeout(
    pal_scan_probe_t* task
)
{
    dbg_assert_ptr(task->scan);
    dbg_assert_is_task(task->scan->scheduler);
    pal_scan_probe_complete(task);
}

//
// Schedule next ports to be scanned
//
static void pal_scan_next_port(
    pal_scan_t* scan
)
{
    int32_t result;
    uint16_t port;

    dbg_assert_ptr(scan);
    dbg_assert_is_task(scan->scheduler);
    for (size_t i = 0; i < _countof(scan->tasks); i++)
    {
        // Find next non-pending task
        if (scan->tasks[i].state != pal_scan_probe_idle)
            continue;

        // Select next port
        if (scan->ip_scan_cur >= scan->ip_scan_end)
        {
            // No more candidates
            if (i == 0)
            {
                // Notify we are done.
                scan->cb(scan->context, 0, er_nomore, NULL);
                log_trace(scan->log, "Port scan completed.");
            }
            return;
        }

        port = (uint16_t)scan->ip_scan_cur++;

        // Perform actual scan action - update address with target and port
        scan->tasks[i].state = pal_scan_probe_working;
        memcpy(&scan->tasks[i].to, &scan->address, sizeof(SOCKADDR_INET));
        if (scan->tasks[i].to.si_family == AF_INET6)
        {
            scan->tasks[i].to.Ipv6.sin6_port = swap_16(port);
            log_debug(scan->log, "Probing [%x:%x:%x:%x:%x:%x:%x:%x]:%d",
                scan->tasks[i].to.Ipv6.sin6_addr.u.Word[0],
                scan->tasks[i].to.Ipv6.sin6_addr.u.Word[1],
                scan->tasks[i].to.Ipv6.sin6_addr.u.Word[2],
                scan->tasks[i].to.Ipv6.sin6_addr.u.Word[3],
                scan->tasks[i].to.Ipv6.sin6_addr.u.Word[4],
                scan->tasks[i].to.Ipv6.sin6_addr.u.Word[5],
                scan->tasks[i].to.Ipv6.sin6_addr.u.Word[6],
                scan->tasks[i].to.Ipv6.sin6_addr.u.Word[7], port);
        }
        else
        {
            dbg_assert(scan->tasks[i].to.si_family == AF_INET, "af wrong");
            scan->tasks[i].to.Ipv4.sin_port = swap_16(port);
            log_debug(scan->log, "Probing %d.%d.%d.%d:%d",
                scan->tasks[i].to.Ipv4.sin_addr.S_un.S_un_b.s_b1,
                scan->tasks[i].to.Ipv4.sin_addr.S_un.S_un_b.s_b2,
                scan->tasks[i].to.Ipv4.sin_addr.S_un.S_un_b.s_b3,
                scan->tasks[i].to.Ipv4.sin_addr.S_un.S_un_b.s_b4, port);
        }

        // Connect to port
        memset(&scan->tasks[i].ov, 0, sizeof(OVERLAPPED));
        scan->tasks[i].from.si_family = scan->tasks[i].to.si_family;
        result = pal_socket_create_bind_and_connect_async(
            scan->tasks[i].to.si_family,
            (const struct sockaddr*)&scan->tasks[i].from,
            scan->tasks[i].from.si_family == AF_INET6 ?
                sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in),
            (const struct sockaddr*)&scan->tasks[i].to,
            scan->tasks[i].to.si_family == AF_INET6 ?
                sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in),
            &scan->tasks[i].ov, pal_scan_result_from_OVERLAPPED,
            &scan->tasks[i].socket);
        if (result != er_ok)
        {
            // Failed to connect, continue;
            log_trace(scan->log, "Failed to call connect (%s)",
                prx_err_string(result));
            __do_next_s(scan->scheduler, pal_scan_probe_complete,
                &scan->tasks[i]);
            continue;
        }

        // Schedule timeout of this task
        __do_later_s(scan->scheduler, pal_scan_probe_timeout,
            &scan->tasks[i], PROBE_TIMEOUT);
    }
}

//
// Schedule next addresses to be scanned
//
static void pal_scan_next_address(
    pal_scan_t* scan
)
{
    int32_t result;
    MIB_IPNET_ROW2* row;
    uint32_t subnet_mask;
    PIP_ADAPTER_ADDRESSES ifa;
    PIP_ADAPTER_UNICAST_ADDRESS uai;
    SOCKADDR_INET *to, *from;

    dbg_assert_ptr(scan);
    dbg_assert_is_task(scan->scheduler);

    for (size_t i = 0; i < _countof(scan->tasks); i++)
    {
        // Find next non-pending task
        if (scan->tasks[i].state != pal_scan_probe_idle)
            continue;

        to = &scan->tasks[i].to;
        from = &scan->tasks[i].from;
        while (true)
        {
            if (scan->ip_scan_cur < scan->ip_scan_end)
            {
                scan->ip_scan_cur++;
                to->si_family = to->Ipv4.sin_family = AF_INET;  // Redundant
                to->Ipv4.sin_addr.s_addr = swap_32(scan->ip_scan_cur);

                from->si_family = from->Ipv4.sin_family = AF_INET;  // Redundant
                from->Ipv4.sin_addr.s_addr = scan->ip_scan_itf;
                break;
            }

            // Select candidate address
            if (scan->neighbors)
            {
                if (scan->neighbors->NumEntries == scan->neighbors_index)
                {
                    FreeMibTable(scan->neighbors);
                    scan->neighbors = NULL;
                }
                else
                {
                    row = &scan->neighbors->Table[scan->neighbors_index++];
                    if (row->IsRouter)
                        continue;

                    scan->tasks[i].itf_index = (int32_t)row->InterfaceIndex;
                    memcpy(to, &row->Address, sizeof(SOCKADDR_INET));

                    // Get adapter address to bind to
                    from->si_family = AF_UNSPEC;
                    for (ifa = scan->ifaddr; ifa != NULL; ifa = ifa->Next)
                    {
                        if (ifa->IfIndex != row->InterfaceIndex)
                            continue;
                        if (IfOperStatusUp != ifa->OperStatus)
                            break;

                        for (uai = ifa->FirstUnicastAddress; uai; uai = uai->Next)
                        {
                            if (to->si_family != uai->Address.lpSockaddr->sa_family)
                                continue;
                            memcpy(from, uai->Address.lpSockaddr, uai->Address.iSockaddrLength);
                            break;
                        }

                        if (from->si_family != AF_UNSPEC)
                            break; // Found family address

                        log_debug(scan->log, "Failed to find suitable interface address.");
                        // Continue anyway using "any".
                        memset(from, 0, sizeof(SOCKADDR_INET));
                        from->si_family = to->si_family;
                        break;
                    }

                    if (from->si_family == AF_UNSPEC)
                        continue; // No address found

                    break;
                }
            }

            // See if we can set next range from current unicast address
            if (scan->uacur)
            {
                subnet_mask = (~0 << scan->uacur->OnLinkPrefixLength);
                if (scan->uacur->Address.lpSockaddr->sa_family == AF_INET)
                {
                    scan->ip_scan_itf = ((struct sockaddr_in*)
                        scan->uacur->Address.lpSockaddr)->sin_addr.s_addr;

                    scan->ip_scan_end = scan->ip_scan_itf | subnet_mask;
                    scan->ip_scan_cur = scan->ip_scan_itf & ~subnet_mask;

                    scan->ip_scan_cur = swap_32(scan->ip_scan_cur);
                    scan->ip_scan_end = swap_32(scan->ip_scan_end);
                    scan->ip_scan_end++;

                    log_trace(scan->log, "Scanning %d.%d.%d.%d/%d.",
                        ((uint8_t*)&scan->ip_scan_itf)[0], ((uint8_t*)&scan->ip_scan_itf)[1],
                        ((uint8_t*)&scan->ip_scan_itf)[2], ((uint8_t*)&scan->ip_scan_itf)[3],
                        scan->uacur->OnLinkPrefixLength);
                }
                scan->uacur = scan->uacur->Next;
                continue;
            }

            if (scan->ifcur)
            {
                log_trace(scan->log,
                    "-> %S (%S) (type:%d, flags:%x, status:%d)",
                    scan->ifcur->FriendlyName, scan->ifcur->Description,
                    scan->ifcur->IfType, scan->ifcur->Flags, scan->ifcur->OperStatus);
                if (IfOperStatusUp == scan->ifcur->OperStatus)
                {
                    if (scan->ifcur->IfType == IF_TYPE_ETHERNET_CSMACD ||
                        scan->ifcur->IfType == IF_TYPE_IEEE80211)
                    {
                        scan->uacur = scan->ifcur->FirstUnicastAddress;
                    }
                }
                scan->ifcur = scan->ifcur->Next; // Next adapter
                continue;
            }

            // No more candidates
            if (i == 0)
            {
                // Notify we are done.
                scan->cb(scan->context, 0, er_nomore, NULL);
                log_trace(scan->log, "IP scan completed.");
            }
            return;
        }

        // Perform actual scan action
        if (scan->port)
        {
            scan->tasks[i].state = pal_scan_probe_working;
            // Update address to add port
            if (to->si_family == AF_INET6)
            {
                to->Ipv6.sin6_port = swap_16(scan->port);

                log_debug(scan->log,
                    "Connect on [%x:%x:%x:%x:%x:%x:%x:%x]:%d to [%x:%x:%x:%x:%x:%x:%x:%x]:%d",
                    from->Ipv6.sin6_addr.u.Word[0], from->Ipv6.sin6_addr.u.Word[1],
                    from->Ipv6.sin6_addr.u.Word[2], from->Ipv6.sin6_addr.u.Word[3],
                    from->Ipv6.sin6_addr.u.Word[4], from->Ipv6.sin6_addr.u.Word[5],
                    from->Ipv6.sin6_addr.u.Word[6], from->Ipv6.sin6_addr.u.Word[7],
                    swap_16(from->Ipv6.sin6_port),
                    to->Ipv6.sin6_addr.u.Word[0], to->Ipv6.sin6_addr.u.Word[1],
                    to->Ipv6.sin6_addr.u.Word[2], to->Ipv6.sin6_addr.u.Word[3],
                    to->Ipv6.sin6_addr.u.Word[4], to->Ipv6.sin6_addr.u.Word[5],
                    to->Ipv6.sin6_addr.u.Word[6], to->Ipv6.sin6_addr.u.Word[7], scan->port);
            }
            else
            {
                dbg_assert(to->si_family == AF_INET, "af wrong");
                to->Ipv4.sin_port = swap_16(scan->port);

                log_debug(scan->log, "Connect on %d.%d.%d.%d:%d to %d.%d.%d.%d:%d",
                    from->Ipv4.sin_addr.S_un.S_un_b.s_b1, 
                    from->Ipv4.sin_addr.S_un.S_un_b.s_b2,
                    from->Ipv4.sin_addr.S_un.S_un_b.s_b3, 
                    from->Ipv4.sin_addr.S_un.S_un_b.s_b4,
                    swap_16(from->Ipv4.sin_port),
                    to->Ipv4.sin_addr.S_un.S_un_b.s_b1, 
                    to->Ipv4.sin_addr.S_un.S_un_b.s_b2,
                    to->Ipv4.sin_addr.S_un.S_un_b.s_b3, 
                    to->Ipv4.sin_addr.S_un.S_un_b.s_b4, scan->port);
            }

            // Connect to port
            memset(&scan->tasks[i].ov, 0, sizeof(OVERLAPPED));
            result = pal_socket_create_bind_and_connect_async(
                to->si_family, (const struct sockaddr*)from,
                from->si_family == AF_UNSPEC ? 0 : from->si_family == AF_INET6 ?
                sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in),
                (const struct sockaddr*)to, to->si_family == AF_INET6 ?
                sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in),
                &scan->tasks[i].ov, pal_scan_result_from_OVERLAPPED,
                &scan->tasks[i].socket);
            if (result != er_ok)
            {
                // Failed to connect, continue;
                log_trace(scan->log, "Failed to call connect (%s)",
                    prx_err_string(result));
                __do_next_s(scan->scheduler, pal_scan_probe_complete,
                    &scan->tasks[i]);
                continue;
            }

            // Schedule timeout of this task
            __do_later_s(scan->scheduler, pal_scan_probe_timeout,
                &scan->tasks[i], PROBE_TIMEOUT);
        }
        else
        {
            if (to->si_family == AF_INET6)
            {
                continue;
            }

            dbg_assert(to->si_family == AF_INET, "af wrong");
            scan->tasks[i].state = pal_scan_probe_working;
            if (!QueueUserWorkItem(pal_scan_probe_with_arp, &scan->tasks[i], 0))
            {
                result = pal_os_last_error_as_prx_error();
                log_error(scan->log, "Failed to queue arp request (%s)",
                    prx_err_string(result));

                result = er_ok;
                continue;
            }
        }
    }
}

//
// Free scan - once created is called on scheduler thread.
//
static void pal_scan_free(
    pal_scan_t* scan
)
{
    dbg_assert_ptr(scan);
    scan->closed = true;

    if (scan->scheduler)
        prx_scheduler_clear(scan->scheduler, NULL, scan);

    for (size_t i = 0; i < _countof(scan->tasks); i++)
    {
        if (scan->tasks[i].state == pal_scan_probe_idle)
            continue;

        //
        // Cannot cancel threadpool task.  Wait for arp 
        // to complete then come back...
        //
        if (scan->tasks[i].socket == _invalid_fd)
            return;

        pal_scan_probe_cancel(&scan->tasks[i]);
    }

    // All tasks are idle, now we can free...

    if (scan->scheduler)
        prx_scheduler_release(scan->scheduler, scan);
    if (scan->ifaddr)
        mem_free(scan->ifaddr);
    if (scan->neighbors)
        FreeMibTable(scan->neighbors);

    log_trace(scan->log, "Scan %p closed.", scan);
    mem_free_type(pal_scan_t, scan);
}

//
// Schedule next set of probe task for tasks that have completed
//
static void pal_scan_next(
    pal_scan_t* scan
)
{
    dbg_assert_ptr(scan);
    dbg_assert_is_task(scan->scheduler);

    if (scan->closed) 
    {
        // If scan is closed, continue here by freeing it.
        pal_scan_free(scan);
        return;
    }
    
    if (scan->address.si_family == AF_UNSPEC)
        pal_scan_next_address(scan);
    else
        pal_scan_next_port(scan);

    // Clear scheduler since we have filled all empty tasks
    prx_scheduler_clear(scan->scheduler, (prx_task_t)pal_scan_next, scan);
}

//
// Create scan context
//
static int32_t pal_scan_create(
    prx_scheduler_t* parent,
    int32_t flags,
    pal_scan_cb_t cb,
    void* context,
    pal_scan_t** created
)
{
    int32_t result;
    pal_scan_t* scan;

    chk_arg_fault_return(cb);

    scan = (pal_scan_t*)mem_zalloc_type(pal_scan_t);
    if (!scan)
        return er_out_of_memory;
    do
    {
        // Initialize scan
        scan->log = log_get("pal.scan");
        scan->flags = flags;
        scan->cb = cb;
        scan->context = context;

        for (size_t i = 0; i < _countof(scan->tasks); i++)
        {
            scan->tasks[i].state = pal_scan_probe_idle;
            scan->tasks[i].socket = _invalid_fd;
            scan->tasks[i].scan = scan;
        }

        result = prx_scheduler_create(parent, &scan->scheduler);
        if (result != er_ok)
            break;

        *created = scan;
        return er_ok;
    }
    while (0);

    pal_scan_free(scan);
    return result;
}

//
// Scan for addresses with open port in subnet
//
int32_t pal_scan_net(
    int32_t flags,
    uint16_t port,
    pal_scan_cb_t cb,
    void* context,
    pal_scan_t** created
)
{
    int32_t result;
    DWORD error;
    pal_scan_t* scan;
    ULONG alloc_size = 15000;

    chk_arg_fault_return(cb);

    result = pal_scan_create(_scheduler, flags, cb, context, &scan);
    if (result != er_ok)
        return result;
    do
    {
        scan->port = port;

        // a) Get interface info
        while (true)
        {
            scan->ifcur = (PIP_ADAPTER_ADDRESSES)mem_realloc(
                scan->ifaddr, alloc_size);
            if (!scan->ifcur)
            {
                result = er_out_of_memory;
                break;
            }

            scan->ifaddr = scan->ifcur;
            error = GetAdaptersAddresses(AF_UNSPEC,
                GAA_FLAG_INCLUDE_PREFIX |
                GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_FRIENDLY_NAME |
                GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                NULL, scan->ifaddr, &alloc_size);

            if (error == ERROR_BUFFER_OVERFLOW)
                continue;

            if (error != 0)
            {
                result = pal_os_to_prx_error(error);
                log_error(scan->log, "Failed to get adapter infos (%x, %s).",
                    error, prx_err_string(result));
                break;
            }
            result = er_ok;
            break;
        }

        if (result != er_ok)
            break;

        // b) Get neighbor table for ipv6 addresses
        error = GetIpNetTable2(AF_UNSPEC, &scan->neighbors);
        if (error != 0)
        {
            result = pal_os_to_prx_net_error(error);
            log_error(scan->log, "Failure to get neighbor table (%x, %s).",
                error, prx_err_string(result));
            break;
        }

        // Start scan
        __do_next(scan, pal_scan_next);
        *created = scan;
        return er_ok;
    }
    while (0);

    pal_scan_free(scan);
    return result;
}

//
// Scan for ports on address
//
int32_t pal_scan_ports(
    const prx_socket_address_t* addr,
    uint16_t port_range_low,
    uint16_t port_range_high,
    int32_t flags,
    pal_scan_cb_t cb,
    void* context,
    pal_scan_t** created
)
{
    int32_t result;
    pal_scan_t* scan;
    socklen_t sa_len;

    chk_arg_fault_return(addr);
    chk_arg_fault_return(cb);

    if (!port_range_low)
        port_range_low = 1;
    if (!port_range_high)
        port_range_high = (uint16_t)-1;
    if (port_range_high <= port_range_low)
        return er_arg;

    result = pal_scan_create(_scheduler, flags, cb, context, &scan);
    if (result != er_ok)
        return result;
    do
    {
        scan->ip_scan_cur = port_range_low;
        scan->ip_scan_end = port_range_high;
        scan->ip_scan_end++;

        sa_len = sizeof(SOCKADDR_INET);
        result = pal_os_from_prx_socket_address(addr,
            (struct sockaddr*)&scan->address, &sa_len);
        if (result != er_ok)
            break;

        // Start scan
        __do_next(scan, pal_scan_next);
        *created = scan;
        return er_ok;
    }
    while (0);

    pal_scan_free(scan);
    return result;
}

//
// Abort and close in progress scan
//
void pal_scan_close(
    pal_scan_t* scan
)
{
    // Detach callback
    scan->cb = pal_scan_dummy_cb;
    scan->closed = true;  // Destroy
    __do_next(scan, pal_scan_free);
}

//
// Returns the socket scheduler - used by pal_net_win for scanning
//
#if defined(USE_SK_PARENT)
extern prx_scheduler_t* pal_socket_scheduler(
    void
);
#endif

//
// Called before using scan layer
//
int32_t pal_scan_init(
    void
)
{
    int32_t result;
    prx_scheduler_t* parent;
    if (_scheduler)
        return er_bad_state;
#if defined(USE_SK_PARENT)
    parent = pal_socket_scheduler();
#else
    parent = NULL;
#endif
    result = prx_scheduler_create(parent, &_scheduler);
    if (result != er_ok)
        return result;
    return er_ok;
}

//
// Free networking layer
//
void pal_scan_deinit(
    void
)
{
    if (_scheduler)
    {
        prx_scheduler_release(_scheduler, NULL);
#if !defined(USE_SK_PARENT)
        prx_scheduler_at_exit(_scheduler);
#endif
        _scheduler = NULL;
    }
}
