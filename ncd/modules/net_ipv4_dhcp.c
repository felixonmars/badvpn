/**
 * @file net_ipv4_dhcp.c
 * @author Ambroz Bizjak <ambrop7@gmail.com>
 * 
 * @section LICENSE
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the author nor the
 *    names of its contributors may be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 * @section DESCRIPTION
 * 
 * DHCP client module.
 * 
 * Synopsis:
 *   net.ipv4.dhcp(string ifname [, list opts])
 * 
 * Description:
 *   Runs a DHCP client on a network interface. When an address is obtained,
 *   transitions up (but does not assign anything). If the lease times out,
 *   transitions down.
 *   The interface must already be up.
 *   Supported options (in the opts argument):
 *   - "hostname", (string value): send this hostname to the DHCP server
 *   - "vendorclassid", (string value): send this vendor class identifier
 *   - "auto_clientid": send a client identifier generated from the MAC address
 * 
 * Variables:
 *   string addr - assigned IP address ("A.B.C.D")
 *   string prefix - address prefix length ("N")
 *   string cidr_addr - address and prefix in CIDR notation ("A.B.C.D/N")
 *   string gateway - router address ("A.B.C.D"), or "none" if not provided
 *   list(string) dns_servers - DNS server addresses ("A.B.C.D" ...)
 *   string server_mac - MAC address of the DHCP server (6 two-digit caps hexadecimal values
 *     separated with colons, e.g."AB:CD:EF:01:02:03")
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <misc/debug.h>
#include <misc/ipaddr.h>
#include <dhcpclient/BDHCPClient.h>
#include <ncd/NCDModule.h>

#include <generated/blog_channel_ncd_net_ipv4_dhcp.h>

#define ModuleLog(i, ...) NCDModuleInst_Backend_Log((i), BLOG_CURRENT_CHANNEL, __VA_ARGS__)

struct instance {
    NCDModuleInst *i;
    BDHCPClient dhcp;
    int up;
};

static void instance_free (struct instance *o);

static void dhcp_handler (struct instance *o, int event)
{
    switch (event) {
        case BDHCPCLIENT_EVENT_UP: {
            ASSERT(!o->up)
            o->up = 1;
            NCDModuleInst_Backend_Up(o->i);
        } break;
        
        case BDHCPCLIENT_EVENT_DOWN: {
            ASSERT(o->up)
            o->up = 0;
            NCDModuleInst_Backend_Down(o->i);
        } break;
        
        case BDHCPCLIENT_EVENT_ERROR: {
            NCDModuleInst_Backend_SetError(o->i);
            instance_free(o);
            return;
        } break;
        
        default: ASSERT(0);
    }
}

static void func_new (void *vo, NCDModuleInst *i, const struct NCDModuleInst_new_params *params)
{
    struct instance *o = vo;
    o->i = i;
    
    // check arguments
    NCDValRef ifname_arg;
    NCDValRef opts_arg = NCDVal_NewInvalid();
    if (!NCDVal_ListRead(params->args, 1, &ifname_arg) && !NCDVal_ListRead(params->args, 2, &ifname_arg, &opts_arg)) {
        ModuleLog(o->i, BLOG_ERROR, "wrong arity");
        goto fail0;
    }
    if (!NCDVal_IsStringNoNulls(ifname_arg) || (!NCDVal_IsInvalid(opts_arg) && !NCDVal_IsList(opts_arg))) {
        ModuleLog(o->i, BLOG_ERROR, "wrong type");
        goto fail0;
    }
    const char *ifname = NCDVal_StringValue(ifname_arg);
    
    struct BDHCPClient_opts opts = {};
    
    // read options
    size_t count = NCDVal_IsInvalid(opts_arg) ? 0 : NCDVal_ListCount(opts_arg);
    for (size_t j = 0; j < count; j++) {
        NCDValRef opt = NCDVal_ListGet(opts_arg, j);
        
        // read name
        if (!NCDVal_IsStringNoNulls(opt)) {
            ModuleLog(o->i, BLOG_ERROR, "wrong option name type");
            goto fail0;
        }
        const char *optname = NCDVal_StringValue(opt);
        
        if (!strcmp(optname, "hostname") || !strcmp(optname, "vendorclassid")) {
            // read value
            if (j == count) {
                ModuleLog(o->i, BLOG_ERROR, "option value missing");
                goto fail0;
            }
            NCDValRef val = NCDVal_ListGet(opts_arg, j + 1);
            if (!NCDVal_IsStringNoNulls(val)) {
                ModuleLog(o->i, BLOG_ERROR, "wrong option value type");
                goto fail0;
            }
            const char *optval = NCDVal_StringValue(val);
            
            if (!strcmp(optname, "hostname")) {
                opts.hostname = optval;
            } else {
                opts.vendorclassid = optval;
            }
            
            j++;
        }
        else if (!strcmp(optname, "auto_clientid")) {
            opts.auto_clientid = 1;
        }
        else {
            ModuleLog(o->i, BLOG_ERROR, "unknown option name");
            goto fail0;
        }
    }
    
    // init DHCP
    if (!BDHCPClient_Init(&o->dhcp, ifname, opts, o->i->params->iparams->reactor, o->i->params->iparams->random2, (BDHCPClient_handler)dhcp_handler, o)) {
        ModuleLog(o->i, BLOG_ERROR, "BDHCPClient_Init failed");
        goto fail0;
    }
    
    // set not up
    o->up = 0;
    return;
    
fail0:
    NCDModuleInst_Backend_SetError(i);
    NCDModuleInst_Backend_Dead(i);
}

static void instance_free (struct instance *o)
{
    // free DHCP
    BDHCPClient_Free(&o->dhcp);
    
    NCDModuleInst_Backend_Dead(o->i);
}

static void func_die (void *vo)
{
    struct instance *o = vo;
    
    instance_free(o);
}

static int func_getvar (void *vo, const char *name, NCDValMem *mem, NCDValRef *out)
{
    struct instance *o = vo;
    ASSERT(o->up)
    
    if (!strcmp(name, "addr")) {
        uint32_t addr;
        BDHCPClient_GetClientIP(&o->dhcp, &addr);
        
        char str[IPADDR_PRINT_MAX];
        ipaddr_print_addr(addr, str);
        
        *out = NCDVal_NewString(mem, str);
        if (NCDVal_IsInvalid(*out)) {
            ModuleLog(o->i, BLOG_ERROR, "NCDVal_NewString failed");
        }
        return 1;
    }
    
    if (!strcmp(name, "prefix")) {
        uint32_t addr;
        BDHCPClient_GetClientIP(&o->dhcp, &addr);
        uint32_t mask;
        BDHCPClient_GetClientMask(&o->dhcp, &mask);
        
        struct ipv4_ifaddr ifaddr;
        if (!ipaddr_ipv4_ifaddr_from_addr_mask(addr, mask, &ifaddr)) {
            ModuleLog(o->i, BLOG_ERROR, "bad netmask");
            return 0;
        }
        
        char str[10];
        sprintf(str, "%d", ifaddr.prefix);
        
        *out = NCDVal_NewString(mem, str);
        if (NCDVal_IsInvalid(*out)) {
            ModuleLog(o->i, BLOG_ERROR, "NCDVal_NewString failed");
        }
        return 1;
    }
    
    if (!strcmp(name, "cidr_addr")) {
        uint32_t addr;
        BDHCPClient_GetClientIP(&o->dhcp, &addr);
        uint32_t mask;
        BDHCPClient_GetClientMask(&o->dhcp, &mask);
        
        struct ipv4_ifaddr ifaddr;
        if (!ipaddr_ipv4_ifaddr_from_addr_mask(addr, mask, &ifaddr)) {
            ModuleLog(o->i, BLOG_ERROR, "bad netmask");
            return 0;
        }
        
        char str[IPADDR_PRINT_MAX];
        ipaddr_print_ifaddr(ifaddr, str);
        
        *out = NCDVal_NewString(mem, str);
        if (NCDVal_IsInvalid(*out)) {
            ModuleLog(o->i, BLOG_ERROR, "NCDVal_NewString failed");
        }
        return 1;
    }
    
    if (!strcmp(name, "gateway")) {
        char str[IPADDR_PRINT_MAX];
        
        uint32_t addr;
        if (!BDHCPClient_GetRouter(&o->dhcp, &addr)) {
            strcpy(str, "none");
        } else {
            ipaddr_print_addr(addr, str);
        }
        
        *out = NCDVal_NewString(mem, str);
        if (NCDVal_IsInvalid(*out)) {
            ModuleLog(o->i, BLOG_ERROR, "NCDVal_NewString failed");
        }
        return 1;
    }
    
    if (!strcmp(name, "dns_servers")) {
        uint32_t servers[BDHCPCLIENT_MAX_DOMAIN_NAME_SERVERS];
        int num_servers = BDHCPClient_GetDNS(&o->dhcp, servers, BDHCPCLIENT_MAX_DOMAIN_NAME_SERVERS);
        
        *out = NCDVal_NewList(mem, num_servers);
        if (NCDVal_IsInvalid(*out)) {
            ModuleLog(o->i, BLOG_ERROR, "NCDVal_NewList failed");
            goto fail;
        }
        
        for (int i = 0; i < num_servers; i++) {
            char str[IPADDR_PRINT_MAX];
            ipaddr_print_addr(servers[i], str);
            
            NCDValRef server = NCDVal_NewString(mem, str);
            if (NCDVal_IsInvalid(server)) {
                ModuleLog(o->i, BLOG_ERROR, "NCDVal_IsInvalid failed");
                goto fail;
            }
            
            NCDVal_ListAppend(*out, server);
        }
        
        return 1;
    }
    
    if (!strcmp(name, "server_mac")) {
        uint8_t mac[6];
        BDHCPClient_GetServerMAC(&o->dhcp, mac);
        
        char str[18];
        sprintf(str, "%02"PRIX8":%02"PRIX8":%02"PRIX8":%02"PRIX8":%02"PRIX8":%02"PRIX8,
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        
        *out = NCDVal_NewString(mem, str);
        if (NCDVal_IsInvalid(*out)) {
            ModuleLog(o->i, BLOG_ERROR, "NCDVal_NewString failed");
        }
        return 1;
    }
    
    return 0;
    
fail:
    *out = NCDVal_NewInvalid();
    return 1;
}

static struct NCDModule modules[] = {
    {
        .type = "net.ipv4.dhcp",
        .func_new2 = func_new,
        .func_die = func_die,
        .func_getvar = func_getvar,
        .alloc_size = sizeof(struct instance)
    }, {
        .type = NULL
    }
};

const struct NCDModuleGroup ncdmodule_net_ipv4_dhcp = {
    .modules = modules
};
