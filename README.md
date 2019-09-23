# Introduction
This repo contains example code to make DPDK application power efficient without introducing much latency increase.
The example application we choose is a software router with access control. The prototype of the application is the
"L3 Forwarding with Access Control" of DPDK sample app:

    https://github.com/DPDK/dpdk/tree/master/examples/l3fwd-acl.

Two folds are include in this repo: l3fwd-acl-tkpe and l3fwd-acl-tupe. They respectively show the  power-conserving
strategies TKPE-DPDK and TUPE-DPDK we proposed in ICPP'18 paper:

    X. Li, W. Cheng, T. Zhang, J. Xie, F. Ren, and B. Yang. "Power efficient high performance packet I/O". In Proc. ICPP'18. ACM, 2018.

# Work to do before make and run the program
1. download and build the dpdk source code (follow the instruction in dpdk mainpage)
2. correctly set the environment virables RTE_SDK:

    export RTE_SDK=path_to_your_root_dir_of_dpdk.

3. Insert IGB_UIO module, setup hugepage, bind the dpdk-supported Ethernet devices to IGB_UIO module

# l3fwd-acl-tkpe
  Term "tkpe" stands for "traffic known power efficient", so l3fwd-acl-tkpe requires historical traffic information to assist in power-conserving decision.
* usage:

    ./build/l3-acl-tkpe [EAL options] -- -p PORTMASK -P  -E Timer_Num_Per_Epoch \
  
           --rule_ipv4="FILE_PATH": specify the ipv4 rules entries file \
           
           --rule_ipv6="FILE_PATH": specify the ipv6 rules entries file\
           
           --history_traffic="FILE_PATH": specify the historical traffic load entriesfile, 1 double value (in kpps) per-line \
           
           --sample_interval=Interger: in Seconds, the traffic sampling interval \
           
           --avg_batch_size=Double: average batch size of arrival traffic\
           
           --start_epoch=Interger: designate the starting index of history_traffic when starting run the program; It helps determine 'what time it is'.\
           
           --app_cycles=Interger: per-packet application processing cycles \
           
           --moving_average=Double: param of moving_average to estimatecalu traffic: e_traffic=ma*his_traffic_load +(1-ma)*traffic_load_just_before \
           
           --config="(port,queue,lcore)[,(port,queue,lcore]]" \
           
           --scalar: Use scalar function to do lookup
           
           ----------------------------------------------------------
           -p PORTMASK: hexadecimal bitmask of ports to configure.
           
           -P : enable promiscuous mode.
           
           -E : timer num per epoch (timer period = sample_interval/timer_num_per_epoch).
           
  
# l3fwd-acl-tupe
Term "tupe" stands for "traffic uknown power efficient", so l3fwd-acl-tupe dose not require historical traffic information to assist in power-conserving decision.
* usage:

    ./build/l3-acl-tkpe [EAL options] -- -p PORTMASK -P  -T Timer_Num_Per_Second \
  
           --rule_ipv4="FILE_PATH": specify the ipv4 rules entries file \
           
           --rule_ipv6="FILE_PATH": specify the ipv6 rules entries file\
                      
           --config="(port,queue,lcore)[,(port,queue,lcore]]" \
           
           --scalar: Use scalar function to do lookup
           
           ----------------------------------------------------------
           -p PORTMASK: hexadecimal bitmask of ports to configure
           
           -P : enable promiscuous mode
           
           -T : timer num per seconds

# About ACL rules and route entries
The ipv4 rules file include the ipv4 ACL items and ipv4 route entries, and ipv6 rules file include the ipv6 ACL items and ipv6 route entries.

To learn he ACL and route rule syntax, please refer to:

   http://doc.dpdk.org/guides/sample_app_ug/l3_forward_access_ctrl.html.

