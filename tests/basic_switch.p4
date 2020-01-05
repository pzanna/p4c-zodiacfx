/* -*- P4_16 -*- */
#include <core.p4>
#include <zodiacfx_model.p4>

const bit<16> TYPE_IPV4 = 0x800;

/*************************************************************************
*********************** H E A D E R S  ***********************************
*************************************************************************/

typedef bit<9>  egressSpec_t;
typedef bit<48> macAddr_t;
typedef bit<32> ip4Addr_t;

header ethernet_t {
    macAddr_t dstAddr;
    macAddr_t srcAddr;
    bit<16>   etherType;
}

header ipv4_t {
    bit<4>    version;
    bit<4>    ihl;
    bit<8>    diffserv;
    bit<16>   totalLen;
    bit<16>   identification;
    bit<3>    flags;
    bit<13>   fragOffset;
    bit<8>    ttl;
    bit<8>    protocol;
    bit<16>   hdrChecksum;
    ip4Addr_t srcAddr;
    ip4Addr_t dstAddr;
}

struct Headers_t {
    ethernet_t ethernet;
    ipv4_t     ipv4;
}

/*************************************************************************
*********************** P A R S E R  ***********************************
*************************************************************************/

parser MyParser(packet_in packet, out Headers_t headers) {
    state start {
        packet.extract(headers.ethernet);
        transition select(headers.ethernet.etherType) {
            16w0x800 : ip;
            default : accept;
        }
    }

    state ip {
        packet.extract(headers.ipv4);
        transition accept;
    }
}

/*************************************************************************
**************************  S W I T C H  *******************************
*************************************************************************/

control swtch(inout Headers_t headers, in zodiacfx_input fxin, out zodiacfx_output fxout){
    
    action drop()
    {
        fxout.output_port = 0;
    }

    action eth_forward(macAddr_t dstAddr, egressSpec_t port)
    {
        headers.ethernet.srcAddr = headers.ethernet.dstAddr;
        headers.ethernet.dstAddr = dstAddr;
    }

    table eth_exact
    {
        key = { headers.ethernet.dstAddr : exact;
                headers.ethernet.etherType: exact;}

        actions = {
            eth_forward;
            drop;
        }

        default_action = drop();

        const entries = {
                (112233445566, 0x08 ) : drop(); 
            }
    }

    apply
    {
        eth_exact.apply();
    }

}

/*************************************************************************
***********************  D E P A R S E R  *******************************
*************************************************************************/

control MyDeparser(in Headers_t headers, packet_out packet, in zodiacfx_output fxout) {
    apply {
        packet.emit(headers.ethernet);
        packet.emit(headers.ipv4);
    }
}

ZodiacfxSwitch(
MyParser(),
swtch(),
MyDeparser()
)main;
