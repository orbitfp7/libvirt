#!/usr/bin/perl -w
#
# This script parses remote_protocol.x or qemu_protocol.x and produces lots of
# boilerplate code for both ends of the remote connection.
#
# The first non-option argument specifies the prefix to be searched for, and
# output to, the boilerplate code.  The second non-option argument is the
# file you want to operate on.  For instance, to generate the dispatch table
# for both remote_protocol.x and qemu_protocol.x, you would run the
# following:
#
# remote_generator.pl -c -t remote ../src/remote/remote_protocol.x
# remote_generator.pl -t qemu ../src/remote/qemu_protocol.x
#
# By Richard Jones <rjones@redhat.com>
# Extended by Matthias Bolte <matthias.bolte@googlemail.com>

use strict;

use Getopt::Std;

# Command line options.
our ($opt_p, $opt_t, $opt_a, $opt_r, $opt_d, $opt_c, $opt_b);
getopts ('ptardcb');

my $structprefix = $ARGV[0];
my $procprefix = uc $structprefix;
shift;

# Convert name_of_call to NameOfCall.
sub name_to_ProcName {
    my $name = shift;
    my @elems = split /_/, $name;
    @elems = map ucfirst, @elems;
    @elems = map { $_ =~ s/Nwfilter/NWFilter/; $_ =~ s/Xml/XML/;
                   $_ =~ s/Uri/URI/; $_ =~ s/Uuid/UUID/; $_ =~ s/Id/ID/;
                   $_ =~ s/Mac/MAC/; $_ =~ s/Cpu/CPU/; $_ =~ s/Os/OS/;
                   $_ } @elems;
    join "", @elems
}

# Read the input file (usually remote_protocol.x) and form an
# opinion about the name, args and return type of each RPC.
my ($name, $ProcName, $id, %calls, @calls);

# only generate a close method if -c was passed
if ($opt_c) {
    # REMOTE_PROC_CLOSE has no args or ret.
    $calls{close} = {
        name => "close",
        ProcName => "Close",
        UC_NAME => "CLOSE",
        args => "void",
        ret => "void",
    };
}

my $collect_args_members = 0;
my $collect_ret_members = 0;
my $last_name;

while (<>) {
    if ($collect_args_members) {
        if (/^};/) {
            $collect_args_members = 0;
        } elsif ($_ =~ m/^\s*(.*\S)\s*$/) {
            push(@{$calls{$name}->{args_members}}, $1);
        }
    } elsif ($collect_ret_members) {
        if (/^};/) {
            $collect_ret_members = 0;
        } elsif ($_ =~ m/^\s*(.*\S)\s*$/) {
            push(@{$calls{$name}->{ret_members}}, $1);
        }
    } elsif (/^struct ${structprefix}_(.*)_args/) {
        $name = $1;
        $ProcName = name_to_ProcName ($name);

        die "duplicate definition of ${structprefix}_${name}_args"
            if exists $calls{$name};

        $calls{$name} = {
            name => $name,
            ProcName => $ProcName,
            UC_NAME => uc $name,
            args => "${structprefix}_${name}_args",
            args_members => [],
            ret => "void"
        };

        $collect_args_members = 1;
        $collect_ret_members = 0;
        $last_name = $name;
    } elsif (/^struct ${structprefix}_(.*)_ret/) {
        $name = $1;
        $ProcName = name_to_ProcName ($name);

        if (exists $calls{$name}) {
            $calls{$name}->{ret} = "${structprefix}_${name}_ret";
        } else {
            $calls{$name} = {
                name => $name,
                ProcName => $ProcName,
                UC_NAME => uc $name,
                args => "void",
                ret => "${structprefix}_${name}_ret",
                ret_members => []
            }
        }

        $collect_args_members = 0;
        $collect_ret_members = 1;
        $last_name = $name;
    } elsif (/^struct ${structprefix}_(.*)_msg/) {
        $name = $1;
        $ProcName = name_to_ProcName ($name);

        $calls{$name} = {
            name => $name,
            ProcName => $ProcName,
            UC_NAME => uc $name,
            msg => "${structprefix}_${name}_msg"
        };

        $collect_args_members = 0;
        $collect_ret_members = 0;
    } elsif (/^\s*${procprefix}_PROC_(.*?)\s+=\s+(\d+),?$/) {
        $name = lc $1;
        $id = $2;
        $ProcName = name_to_ProcName ($name);

        $calls[$id] = $calls{$name};

        $collect_args_members = 0;
        $collect_ret_members = 0;
    } else {
        $collect_args_members = 0;
        $collect_ret_members = 0;
    }
}

#----------------------------------------------------------------------
# Output

print <<__EOF__;
/* Automatically generated by remote_generator.pl.
 * Do not edit this file.  Any changes you make will be lost.
 */

__EOF__

# Debugging.
if ($opt_d) {
    my @keys = sort (keys %calls);
    foreach (@keys) {
        print "$_:\n";
        print "        name $calls{$_}->{name} ($calls{$_}->{ProcName})\n";
        print "        $calls{$_}->{args} -> $calls{$_}->{ret}\n";
    }
}

# Prototypes for dispatch functions ("remote_dispatch_prototypes.h").
elsif ($opt_p) {
    my @keys = sort (keys %calls);
    foreach (@keys) {
        # Skip things which are REMOTE_MESSAGE
        next if $calls{$_}->{msg};

        print "static int ${structprefix}Dispatch$calls{$_}->{ProcName}(\n";
        print "    struct qemud_server *server,\n";
        print "    struct qemud_client *client,\n";
        print "    virConnectPtr conn,\n";
        print "    remote_message_header *hdr,\n";
        print "    remote_error *rerr,\n";
        print "    $calls{$_}->{args} *args,\n";
        print "    $calls{$_}->{ret} *ret);\n";
    }
}

# Union of all arg types
# ("remote_dispatch_args.h").
elsif ($opt_a) {
    for ($id = 0 ; $id <= $#calls ; $id++) {
        if (defined $calls[$id] &&
            !$calls[$id]->{msg} &&
            $calls[$id]->{args} ne "void") {
            print "    $calls[$id]->{args} val_$calls[$id]->{args};\n";
        }
    }
}

# Union of all arg types
# ("remote_dispatch_ret.h").
elsif ($opt_r) {
    for ($id = 0 ; $id <= $#calls ; $id++) {
        if (defined $calls[$id] &&
            !$calls[$id]->{msg} &&
            $calls[$id]->{ret} ne "void") {
            print "    $calls[$id]->{ret} val_$calls[$id]->{ret};\n";
        }
    }
}

# Inside the switch statement, prepare the 'fn', 'args_filter', etc
# ("remote_dispatch_table.h").
elsif ($opt_t) {
    for ($id = 0 ; $id <= $#calls ; $id++) {
        if (defined $calls[$id] && !$calls[$id]->{msg}) {
            print "{   /* $calls[$id]->{ProcName} => $id */\n";
            print "    .fn = (dispatch_fn) ${structprefix}Dispatch$calls[$id]->{ProcName},\n";
            if ($calls[$id]->{args} ne "void") {
                print "    .args_filter = (xdrproc_t) xdr_$calls[$id]->{args},\n";
            } else {
                print "    .args_filter = (xdrproc_t) xdr_void,\n";
            }
            if ($calls[$id]->{ret} ne "void") {
                print "    .ret_filter = (xdrproc_t) xdr_$calls[$id]->{ret},\n";
            } else {
                print "    .ret_filter = (xdrproc_t) xdr_void,\n";
            }
            print "},\n";
        } else {
            if ($calls[$id]->{msg}) {
                print "{   /* Async event $calls[$id]->{ProcName} => $id */\n";
            } else {
                print "{   /* (unused) => $id */\n";
            }
            print "    .fn = NULL,\n";
            print "    .args_filter = (xdrproc_t) xdr_void,\n";
            print "    .ret_filter = (xdrproc_t) xdr_void,\n";
            print "},\n";
        }
    }
}

# Bodies for dispatch functions ("remote_dispatch_bodies.c").
elsif ($opt_b) {
    # list of functions that currently are not generatable
    my @ungeneratable;

    if ($structprefix eq "remote") {
        @ungeneratable = ("Close",
                          "DomainEventsDeregisterAny",
                          "DomainEventsRegisterAny",
                          "DomainMigratePrepareTunnel",
                          "DomainOpenConsole",
                          "DomainPinVcpu",
                          "DomainSetSchedulerParameters",
                          "DomainSetMemoryParameters",
                          "DomainSetBlkioParameters",
                          "Open",
                          "StorageVolUpload",
                          "StorageVolDownload",

                          "AuthList",
                          "AuthSaslInit",
                          "AuthSaslStart",
                          "AuthSaslStep",
                          "AuthPolkit",

                          "DomainBlockPeek",
                          "DomainBlockStats",
                          "DomainCreateWithFlags",
                          "DomainEventsDeregister",
                          "DomainEventsRegister",
                          "DomainGetBlkioParameters",
                          "DomainGetBlockInfo",
                          "DomainGetInfo",
                          "DomainGetJobInfo",
                          "DomainGetMemoryParameters",
                          "DomainGetSchedulerParameters",
                          "DomainGetSchedulerType",
                          "DomainGetSecurityLabel",
                          "DomainGetVcpus",
                          "DomainInterfaceStats",
                          "DomainMemoryPeek",
                          "DomainMemoryStats",
                          "DomainMigratePrepare",
                          "DomainMigratePrepare2",
                          "GetType",
                          "NodeDeviceGetParent",
                          "NodeGetInfo",
                          "NodeGetSecurityModel",
                          "SecretGetValue",
                          "StoragePoolGetInfo",
                          "StorageVolGetInfo");
    } elsif ($structprefix eq "qemu") {
        @ungeneratable = ("MonitorCommand");
    }

    my %ug = map { $_ => 1 } @ungeneratable;
    my @keys = sort (keys %calls);

    foreach (@keys) {
        # skip things which are REMOTE_MESSAGE
        next if $calls{$_}->{msg};

        if (exists($ug{$calls{$_}->{ProcName}})) {
            print "\n/* ${structprefix}Dispatch$calls{$_}->{ProcName} has " .
                  "to be implemented manually */\n";
            next;
        }

        print "\n";
        print "static int\n";
        print "${structprefix}Dispatch$calls{$_}->{ProcName}(\n";
        print "    struct qemud_server *server ATTRIBUTE_UNUSED,\n";
        print "    struct qemud_client *client ATTRIBUTE_UNUSED,\n";
        print "    virConnectPtr conn,\n";
        print "    remote_message_header *hdr ATTRIBUTE_UNUSED,\n";
        print "    remote_error *rerr,\n";
        print "    $calls{$_}->{args} *args";

        if ($calls{$_}->{args} eq "void") {
            print " ATTRIBUTE_UNUSED"
        }

        print ",\n";
        print "    $calls{$_}->{ret} *ret";

        if ($calls{$_}->{ret} eq "void") {
            print " ATTRIBUTE_UNUSED"
        }

        print ")\n";
        print "{\n";
        print "    int rv = -1;\n";

        my $has_node_device = 0;
        my @vars_list = ();
        my @optionals_list = ();
        my @getters_list = ();
        my @args_list = ();
        my @ret_list = ();
        my @free_list = ();

        if ($calls{$_}->{args} ne "void") {
            # node device is special, as it's identified by name
            if ($calls{$_}->{args} =~ m/^remote_node_device_/ and
                !($calls{$_}->{args} =~ m/^remote_node_device_lookup_by_name_/) and
                !($calls{$_}->{args} =~ m/^remote_node_device_create_xml_/)) {
                $has_node_device = 1;
                push(@vars_list, "virNodeDevicePtr dev = NULL");
                push(@getters_list,
                     "    if (!(dev = virNodeDeviceLookupByName(conn, args->name)))\n" .
                     "        goto cleanup;\n");
                push(@args_list, "dev");
                push(@free_list,
                     "    if (dev)\n" .
                     "        virNodeDeviceFree(dev);");
            }

            foreach my $args_member (@{$calls{$_}->{args_members}}) {
                if ($args_member =~ m/^remote_nonnull_string name;/ and $has_node_device) {
                    # ignore the name arg for node devices
                    next
                } elsif ($args_member =~ m/^remote_nonnull_(domain|network|storage_pool|storage_vol|interface|secret|nwfilter) (\S+);/) {
                    my $type_name = name_to_ProcName($1);

                    push(@vars_list, "vir${type_name}Ptr $2 = NULL");
                    push(@getters_list,
                         "    if (!($2 = get_nonnull_$1(conn, args->$2)))\n" .
                         "        goto cleanup;\n");
                    push(@args_list, "$2");
                    push(@free_list,
                         "    if ($2)\n" .
                         "        vir${type_name}Free($2);");
                } elsif ($args_member =~ m/^remote_nonnull_domain_snapshot /) {
                    push(@vars_list, "virDomainPtr dom = NULL");
                    push(@vars_list, "virDomainSnapshotPtr snapshot = NULL");
                    push(@getters_list,
                         "    if (!(dom = get_nonnull_domain(conn, args->snap.dom)))\n" .
                         "        goto cleanup;\n" .
                         "\n" .
                         "    if (!(snapshot = get_nonnull_domain_snapshot(dom, args->snap)))\n" .
                         "        goto cleanup;\n");
                    push(@args_list, "snapshot");
                    push(@free_list,
                         "    if (snapshot)\n" .
                         "        virDomainSnapshotFree(snapshot);\n" .
                         "    if (dom)\n" .
                         "        virDomainFree(dom);");
                } elsif ($args_member =~ m/(\S+)<\S+>;/) {
                    if (! @args_list) {
                        push(@args_list, "conn");
                    }

                    if ($calls{$_}->{ProcName} eq "SecretSetValue") {
                        push(@args_list, "(const unsigned char *)args->$1.$1_val");
                    } elsif ($calls{$_}->{ProcName} eq "CPUBaseline") {
                        push(@args_list, "(const char **)args->$1.$1_val");
                    } else {
                        push(@args_list, "args->$1.$1_val");
                    }

                    push(@args_list, "args->$1.$1_len");
                } elsif ($args_member =~ m/(\S+) (\S+);/) {
                    if (! @args_list) {
                        push(@args_list, "conn");
                    }

                    if ($1 eq "remote_uuid") {
                        push(@args_list, "(unsigned char *) args->$2");
                    } elsif ($1 eq "remote_string") {
                        push(@vars_list, "char *$2");
                        push(@optionals_list, "$2");
                        push(@args_list, "$2");
                    } else {
                        push(@args_list, "args->$2");
                    }
                }
            }
        }

        my $single_ret_var = "undefined";
        my $single_ret_by_ref = 0;
        my $single_ret_check = " == undefined";
        my $single_ret_as_list = 0;
        my $single_ret_list_name = "undefined";
        my $single_ret_list_max_var = "undefined";
        my $single_ret_list_max_define = "undefined";

        if ($calls{$_}->{ret} ne "void") {
            foreach my $ret_member (@{$calls{$_}->{ret_members}}) {
                if ($ret_member =~ m/remote_nonnull_string (\S+)<(\S+)>;/) {
                    push(@vars_list, "int len");
                    push(@ret_list, "ret->$1.$1_len = len;");
                    push(@free_list,
                         "    if (rv < 0)\n" .
                         "        VIR_FREE(ret->$1.$1_val);");
                    $single_ret_var = "len";
                    $single_ret_by_ref = 0;
                    $single_ret_check = " < 0";
                    $single_ret_as_list = 1;
                    $single_ret_list_name = $1;
                    $single_ret_list_max_var = "max$1";
                    $single_ret_list_max_define = $2;

                    if ($calls{$_}->{ProcName} eq "NodeListDevices") {
                        my $conn = shift(@args_list);
                        my $cap = shift(@args_list);
                        unshift(@args_list, "ret->$1.$1_val");
                        unshift(@args_list, $cap);
                        unshift(@args_list, $conn);
                    } else {
                        my $conn = shift(@args_list);
                        unshift(@args_list, "ret->$1.$1_val");
                        unshift(@args_list, $conn);
                    }
                } elsif ($ret_member =~ m/remote_nonnull_string (\S+);/) {
                    push(@vars_list, "char *$1");
                    push(@ret_list, "ret->$1 = $1;");
                    $single_ret_var = $1;
                    $single_ret_by_ref = 0;
                    $single_ret_check = " == NULL";
                } elsif ($ret_member =~ m/remote_nonnull_(domain|network|storage_pool|storage_vol|interface|node_device|secret|nwfilter|domain_snapshot) (\S+);/) {
                    my $type_name = name_to_ProcName($1);

                    push(@vars_list, "vir${type_name}Ptr $2 = NULL");
                    push(@ret_list, "make_nonnull_$1(&ret->$2, $2);");
                    push(@free_list,
                         "    if ($2)\n" .
                         "        vir${type_name}Free($2);");
                    $single_ret_var = $2;
                    $single_ret_by_ref = 0;
                    $single_ret_check = " == NULL";
                } elsif ($ret_member =~ m/int (\S+)<(\S+)>;/) {
                    push(@vars_list, "int len");
                    push(@ret_list, "ret->$1.$1_len = len;");
                    push(@free_list,
                         "    if (rv < 0)\n" .
                         "        VIR_FREE(ret->$1.$1_val);");
                    $single_ret_var = "len";
                    $single_ret_by_ref = 0;
                    $single_ret_check = " < 0";
                    $single_ret_as_list = 1;
                    $single_ret_list_name = $1;
                    $single_ret_list_max_var = "max$1";
                    $single_ret_list_max_define = $2;

                    my $conn = shift(@args_list);
                    unshift(@args_list, "ret->$1.$1_val");
                    unshift(@args_list, $conn);
                } elsif ($ret_member =~ m/int (\S+);/) {
                    push(@vars_list, "int $1");
                    push(@ret_list, "ret->$1 = $1;");
                    $single_ret_var = $1;

                    if ($calls{$_}->{ProcName} eq "DomainGetAutostart" or
                        $calls{$_}->{ProcName} eq "NetworkGetAutostart" or
                        $calls{$_}->{ProcName} eq "StoragePoolGetAutostart") {
                        $single_ret_by_ref = 1;
                    } else {
                        $single_ret_by_ref = 0;

                        if ($calls{$_}->{ProcName} eq "CPUCompare") {
                            $single_ret_check = " == VIR_CPU_COMPARE_ERROR";
                        } else {
                            $single_ret_check = " < 0";
                        }
                    }
                } elsif ($ret_member =~ m/hyper (\S+)<(\S+)>;/) {
                    push(@vars_list, "int len");
                    push(@ret_list, "ret->$1.$1_len = len;");
                    push(@free_list,
                         "    if (rv < 0)\n" .
                         "        VIR_FREE(ret->$1.$1_val);");
                    $single_ret_var = "len";
                    $single_ret_by_ref = 0;
                    $single_ret_as_list = 1;
                    $single_ret_list_name = $1;
                    $single_ret_list_max_define = $2;

                    my $conn = shift(@args_list);

                    if ($calls{$_}->{ProcName} eq "NodeGetCellsFreeMemory") {
                        $single_ret_check = " <= 0";
                        $single_ret_list_max_var = "maxCells";
                        unshift(@args_list, "(unsigned long long *)ret->$1.$1_val");
                    } else {
                        $single_ret_check = " < 0";
                        $single_ret_list_max_var = "max$1";
                        unshift(@args_list, "ret->$1.$1_val");
                    }

                    unshift(@args_list, $conn);
                } elsif ($ret_member =~ m/hyper (\S+);/) {
                    push(@vars_list, "unsigned long $1");
                    push(@ret_list, "ret->$1 = $1;");
                    $single_ret_var = $1;

                    if ($calls{$_}->{ProcName} eq "DomainGetMaxMemory" or
                        $calls{$_}->{ProcName} eq "NodeGetFreeMemory") {
                        $single_ret_by_ref = 0;
                        $single_ret_check = " == 0";
                    } else {
                        $single_ret_by_ref = 1;
                    }
                }
            }
        }

        foreach my $var (@vars_list) {
            print "    $var;\n";
        }

        print "\n";
        print "    if (!conn) {\n";
        print "        virNetError(VIR_ERR_INTERNAL_ERROR, \"%s\", _(\"connection not open\"));\n";
        print "        goto cleanup;\n";
        print "    }\n";
        print "\n";

        if ($single_ret_as_list) {
            print "    if (args->$single_ret_list_max_var > $single_ret_list_max_define) {\n";
            print "        virNetError(VIR_ERR_INTERNAL_ERROR,\n";
            print "                    \"%s\", _(\"max$single_ret_list_name > $single_ret_list_max_define\"));\n";
            print "        goto cleanup;\n";
            print "    }\n";
            print "\n";
        }

        print join("\n", @getters_list);

        if (@getters_list) {
            print "\n";
        }

        foreach my $optional (@optionals_list) {
            print "    $optional = args->$optional ? *args->$optional : NULL;\n";
        }

        if (@optionals_list) {
            print "\n";
        }

        if ($calls{$_}->{ret} eq "void") {
            print "    if (vir$calls{$_}->{ProcName}(";
            print join(', ', @args_list);
            print ") < 0)\n";
            print "        goto cleanup;\n";
            print "\n";
        } elsif (scalar(@{$calls{$_}->{ret_members}}) == 1) {
            my $prefix = "";
            my $proc_name = $calls{$_}->{ProcName};

            if (! @args_list) {
                push(@args_list, "conn");

                if ($calls{$_}->{ProcName} ne "NodeGetFreeMemory") {
                    $prefix = "Connect"
                }
            }

            if ($calls{$_}->{ProcName} eq "GetSysinfo" or
                $calls{$_}->{ProcName} eq "GetMaxVcpus" or
                $calls{$_}->{ProcName} eq "DomainXMLFromNative" or
                $calls{$_}->{ProcName} eq "DomainXMLToNative" or
                $calls{$_}->{ProcName} eq "FindStoragePoolSources" or
                $calls{$_}->{ProcName} =~ m/^List/) {
                $prefix = "Connect"
            } elsif ($calls{$_}->{ProcName} eq "SupportsFeature") {
                $prefix = "Drv"
            } elsif ($calls{$_}->{ProcName} =~ m/^(\S+)DumpXML$/) {
                $proc_name = "${1}GetXMLDesc"
            } elsif ($calls{$_}->{ProcName} eq "CPUBaseline") {
                $proc_name = "ConnectBaselineCPU"
            } elsif ($calls{$_}->{ProcName} eq "CPUCompare") {
                $proc_name = "ConnectCompareCPU"
            }

            if ($single_ret_as_list) {
                print "    /* Allocate return buffer. */\n";
                print "    if (VIR_ALLOC_N(ret->$single_ret_list_name.${single_ret_list_name}_val," .
                      " args->$single_ret_list_max_var) < 0) {\n";
                print "        virReportOOMError();\n";
                print "        goto cleanup;\n";
                print "    }\n";
                print "\n";
            }

            if ($single_ret_by_ref) {
                print "    if (vir$prefix$proc_name(";
                print join(', ', @args_list);
                print ", &$single_ret_var) < 0)\n";
            } else {
                print "    if (($single_ret_var = vir$prefix$proc_name(";
                print join(', ', @args_list);
                print "))$single_ret_check)\n";
            }

            print "        goto cleanup;\n";
            print "\n";

            if (@ret_list) {
                print "    ";
            }

            print join("    \n", @ret_list);
            print "\n";
        }

        print "    rv = 0;\n";
        print "\n";
        print "cleanup:\n";
        print "    if (rv < 0)\n";
        print "        remoteDispatchError(rerr);\n";

        print join("\n", @free_list);

        if (@free_list) {
            print "\n";
        }

        print "    return rv;\n";
        print "}\n";
    }
}
