// mount/unmount a bank [key on/off or off/on through software!]
// Copyright (C) 2007-2017 Harro Verkouter
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// any later version.
// 
// This program is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
// PARTICULAR PURPOSE.  See the GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
// 
// Author:  Harro Verkouter - verkouter@jive.eu
//          Joint Institute for VLBI in Europe
//          P.O. Box 2
//          7990 AA Dwingeloo
#include <mk5_exception.h>
#include <mk5command/mk5.h>
#include <iostream>
#include <set>
#include <signal.h>

using namespace std;


typedef XLR_RETURN_CODE (*mountfn_type)(SSHANDLE, UINT32);
typedef std::set<UINT32> mountlist_type;

// One-shot thread function which does the actual bank switch
struct mountargs {

    mountargs(runtime* rtep, mountlist_type const& mlt, mountfn_type fn):
        rteptr( rtep ),  mount_fn( fn ), banks( mlt )
    { EZASSERT2(rteptr, cmdexception, EZINFO("Don't construct thread args with NULL pointer!"));
      EZASSERT2(mount_fn, cmdexception, EZINFO("mount args constructed with NULL mountfn pointer!")); }

    runtime* const       rteptr;
    const mountfn_type   mount_fn;
    const mountlist_type banks;

    private:
        mountargs();
};


void mount_fn_impl(runtime* const rteptr, mountlist_type const& banks, mountfn_type const mount_fn) {
    // Attempt to do the (un)mount
    static const char       bankChar[] = {'A', 'B', '*'};
    mountlist_type::const_iterator ptr = banks.begin();
    try {
        // On the V100/VXF2 clearchannels is not good enough :-(
        XLRCALL( ::XLRSetMode(rteptr->xlrdev.sshandle(), SS_MODE_SINGLE_CHANNEL) );
        for(; ptr!=banks.end(); ptr++) {
            DEBUG(3, "mount_fn_impl/processing bank " << *ptr << endl)
            XLRCALL( mount_fn(rteptr->xlrdev.sshandle(), *ptr) );
        }
    }
    catch( const std::exception& e ) {
        DEBUG(-1, "mount_fn_impl/failed to do (un)mount " << 
                  (ptr==banks.end() ? bankChar[2] : bankChar[*ptr]) << " - " << e.what() << endl);
        push_error( error_type(1006, string("(un)mount failed - ")+e.what()) );
    }
    try {
        // force a check of mount status
        rteptr->xlrdev.update_mount_status();
    }
    catch( const std::exception& e ) {
        DEBUG(-1, "mount_fn_impl/failed to update mount status - " << e.what() << endl);
    }
    DEBUG(3, "mount_fn_impl/clearing runtime's transfer mode to no_transfer" << endl);
    // In the runtime, set the transfer mode back to no_transfer
    RTEEXEC(*rteptr, rteptr->transfermode = no_transfer);
}

void* mount_thrd(void* args) {
    mountargs*    mount = static_cast<mountargs*>(args);

    if( !mount ) {
        DEBUG(-1, "mount_thrd/Do not start thread function with NULL pointer for arguments" << endl);
        // we cannot put back the runtime's state because we don't have a
        // pointer to the runtime!
        return (void*)0;
    }
    mount_fn_impl(mount->rteptr, mount->banks, mount->mount_fn);
    // Free the storage space - it was allocated using "operator new"
    delete mount;

    return (void*)0;
}




#define BANKID(str) ((str=="A")?((UINT)BANK_A):((str=="B")?((UINT)BANK_B):(UINT)-1))

string mount_fn_bankmode( bool , const vector<string>& , runtime& );
string mount_fn_nonbankmode( bool , const vector<string>& , runtime& );

// High level wrapper
string mount_fn( bool qry, const vector<string>& args, runtime& rte) {
    // This one only handles mount= and unmount=
    if( !(args[0]=="mount" || args[0]=="unmount") )
        return string("!")+args[0]+((qry)?("?"):("="))+" 6 : not handled by this implementation ;";

    // They are really only available as commands
    if( qry )
        return string("!")+args[0]+((qry)?("?"):("="))+" 4 : only available as command ;";

    // Depending on which bankmode we're in defer to the actual mount/unmontfn
    const S_BANKMODE    curbm = rte.xlrdev.bankMode();
    if( curbm==SS_BANKMODE_NORMAL )
        return mount_fn_bankmode(qry, args, rte);
    else if( curbm==SS_BANKMODE_DISABLED )
        return mount_fn_nonbankmode(qry, args, rte);
    else
        return string("!")+args[0]+((qry)?("?"):("="))+" 4 : Neither in bank nor non-bank mode ;";
}



// (Un)Mount in bank mode:
//  mount   = a [ : b : c ...] (Yeah, only two banks but it's easier to
//                              pretend it's a list ...)
//  unmount = a [ : b : c ...]      id.
//
// It should already be pre-verified that this isn't a query so the bool argument
// is unused in here
string mount_fn_bankmode(bool , const vector<string>& args, runtime& rte) {
    ostringstream       reply;
    mountlist_type      banks;
    const transfer_type ctm = rte.transfermode;

    // We require at least one argument!
    EZASSERT2( args.size()>1, Error_Code_8_Exception,
               EZINFO("insufficient number of arguments") );

    // we can already form *this* part of the reply
    reply << '!' << args[0] << '=';

    // Verify that we are eligible to execute in the first place:
    // no mount/unmount command whilst doing *anything* with the disks
    INPROGRESS(rte, reply, streamstorbusy(ctm) || diskunavail(ctm));

    // Collect all the arguments into a list (well, actually it's a set) of
    // banks to (un)mount. Note that we have asserted that there is at least
    // one argument (so "++ begin()" is guaranteed to be valid!)
    for(vector<string>::const_iterator curBank = ++args.begin(); curBank!=args.end(); curBank++) {
        const string bnkStr( ::toupper(*curBank) );
        EZASSERT2(bnkStr=="A" || bnkStr=="B", Error_Code_8_Exception, EZINFO(*curBank << " is not a valid bank - 'A' or 'B'"));
        banks.insert( bnkStr=="A" ? BANK_A : BANK_B );
    }
    rte.transfermode = mounting;
    mount_fn_impl(&rte, banks, (args[0]=="unmount" ? ::XLRDismountBank : ::XLRMountBank) );
    //mount_thrd( new mountargs(&rte, banks, (args[0]=="unmount" ? ::XLRDismountBank : ::XLRMountBank)) );
    reply << " 1; ";
    return reply.str();    
}

string mount_fn_nonbankmode( bool , const vector<string>& args, runtime& ) {
    return string("!")+args[0]+"= 2 : Not implemented in non-bank mode yet";
}
#if 0

// Bank handling commands
// Do both "bank_set" and "bank_info" - most of the logic is identical
// Also handle the execution of disk state query (command is handled in its own function)
string bankinfoset_fn_bankmode( bool qry, const vector<string>& args, runtime& rte) {
    const unsigned int  inactive = (unsigned int)-1;
    const string        bl[] = {"A", "B"};
    S_BANKSTATUS        bs[2];
    unsigned int        selected = inactive;
    unsigned int        nactive = 0;
    transfer_type       ctm = rte.transfermode;
    ostringstream       reply;

    // we can already form *this* part of the reply
    reply << "!" << args[0] << ((qry)?('?'):('='));

    // This one only supports "bank_info?" and "bank_set[?=]"
    EZASSERT2( args[0]=="bank_info" || args[0]=="bank_set" || args[0]=="disk_state", Error_Code_6_Exception,
               EZINFO(args[0] << " not supported by this function") );

    // bank_info is only available as query
    if( (args[0]=="bank_info" || args[0]=="disk_state") && !qry ) {
        reply << " 2 : only available as query ;";
        return reply.str();
    }

    // Verify that we are eligible to execute in the first place
    INPROGRESS( rte, reply,
                // no bank_set command whilst doing *anything* with the disks
                (args[0]=="bank_set" && !qry && streamstorbusy(ctm)) ||
                // no query (neither bank_info?/bank_set?) whilst doing bankswitch or condition
                (qry && diskunavail(ctm)) );

    // Ok. Inspect the banksz0rz!
    do_xlr_lock();
    S_DEVSTATUS     dev_status;
    XLR_RETURN_CODE rc = !XLR_SUCCESS;
    XLRCODE( rc=::XLRGetDeviceStatus(GETSSHANDLE(rte), &dev_status); )
    if( rc!=XLR_SUCCESS || dev_status.Playing || dev_status.Recording ) {
        do_xlr_unlock();
        if( rc!=XLR_SUCCESS )
            throw xlrexception("XLRGetDeviceStatus failed");
        reply << " 6 : not whilst " << (dev_status.Playing ? "Playing" : "Recording") << " ;";
        return reply.str();
    }
    XLRCODE( ::XLRGetBankStatus(GETSSHANDLE(rte), BANK_A, &bs[0]) );
    XLRCODE( ::XLRGetBankStatus(GETSSHANDLE(rte), BANK_B, &bs[1]) );
    do_xlr_unlock();
    for(unsigned int bnk=0; bnk<2; bnk++ ) {
        if( bs[bnk].State==STATE_READY ) {
            nactive++;
            if( bs[bnk].Selected ) 
                selected = bnk;
        }
    }
   
    // If we're doing bank_set as a command ...
    // For "bank_set=inc" there's three cases:
    //    0 active banks => return error 6
    //    1 active bank  => return 0 [cyclic rotation to self]
    //    2 active banks => return 1, fire background thread
    if( args[0]=="bank_set" && !qry ) {
        int          code     = 0;
        string       bank_str = ::toupper(OPTARG(1, args));
        const string curbank_str = (selected!=inactive?bl[selected]:"");

        // Not saying which bank is a parameter error (code "8")
        EZASSERT2( bank_str.empty()==false, Error_Code_8_Exception,
                   EZINFO("You must specify which bank to set active" ));
        // can't do inc if the selected bank is inactive. This will be code 6:
        // conflicting request. Note: this also covers the case where
        // 0 banks are active (for bank_set=inc)
        EZASSERT2( (bank_str!="INC") || (bank_str=="INC" && nactive>0 && selected!=inactive),
                   Error_Code_6_Exception, EZINFO("No bank selected so can't toggle using 'inc'") );

        // we've already verified that there *is* a bank selected
        // [if "inc" is requested]
        if( bank_str=="INC" ) {
            if( nactive>1 )
                bank_str = bl[ 1 - selected ];
            else
                bank_str = bl[ selected ];
        }

        ASSERT2_COND( bank_str=="A" || bank_str=="B",
                      SCINFO("invalid bank requested") );

        // If the bank to switch to is not the selected one, we
        // fire up a background thread to do the switch for us
        if( bank_str!=curbank_str ) {
            sigset_t        oss, nss;
            pthread_t       bswitchid;
            pthread_attr_t  tattr;

            code             = 1;
            rte.transfermode = bankswitch;

            // set up for a detached thread with ALL signals blocked
            ASSERT_ZERO( sigfillset(&nss) );
            PTHREAD_CALL( ::pthread_attr_init(&tattr) );
            PTHREAD_CALL( ::pthread_attr_setdetachstate(&tattr, PTHREAD_CREATE_DETACHED) );
            PTHREAD_CALL( ::pthread_sigmask(SIG_SETMASK, &nss, &oss) );
            PTHREAD_CALL( ::pthread_create(&bswitchid, &tattr, bankswitch_thrd, (void*)(new bswitchargs(&rte, BANKID(bank_str)))) );
            // good. put back old sigmask + clean up resources
            PTHREAD_CALL( ::pthread_sigmask(SIG_SETMASK, &oss, 0) );
            PTHREAD_CALL( ::pthread_attr_destroy(&tattr) );
        }
        reply << code << " ;";
        return reply.str();
    }

    // Here we handle the query case for bank_set and bank_info
    reply << " 0 ";
    for(unsigned int i=0; i<2; i++) {
        // output the selected bank first if any
        unsigned int selected_index = i;
        if (selected == 1) {
            selected_index = 1 - i;
        }
        if( bs[selected_index].State!=STATE_READY ) {
            if( args[0]=="bank_info" )
                reply << ": - : 0 ";
            else
                reply << ": - :   ";
        } else {
            const S_BANKSTATUS&  bank( bs[ selected_index ] );
            reply << ": " << bl[ selected_index ] << " : ";
            if( args[0]=="bank_info" ) {
                long page_size = ::sysconf(_SC_PAGESIZE);
                reply << ((bank.TotalCapacity * (uint64_t)page_size) - bank.Length);
            } else {
                pair<string, string> vsn_state = disk_states::split_vsn_state(string(bank.Label));
                if ( args[0] == "bank_set" ) {
                    reply << vsn_state.first;
                }
                else { // disk_state
                    reply << vsn_state.second;
                }
            }
            reply << " ";
        }
    }
    reply << ";";
    return reply.str();
}

// The version for non-bank mode
string bankinfoset_fn_nonbankmode( bool qry, const vector<string>& args, runtime& rte) {
    transfer_type       ctm = rte.transfermode;
    ostringstream       reply;

    // we can already form *this* part of the reply
    reply << "!" << args[0] << ((qry)?('?'):('='));

    // This one only supports "bank_info?" and "bank_set[?=]"
    EZASSERT2( args[0]=="bank_info" || args[0]=="bank_set" || args[0]=="disk_state", Error_Code_6_Exception,
               EZINFO(args[0] << " not supported by this function") );

    // bank_info is only available as query
    if( (args[0]=="bank_info" || args[0]=="disk_state") && !qry ) {
        reply << " 2 : only available as query ;";
        return reply.str();
    }

    // Verify that we are eligible to execute in the first place
    INPROGRESS( rte, reply,
                // no bank_set command whilst doing *anything* with the disks
                (args[0]=="bank_set" && !qry && streamstorbusy(ctm)) ||
                // no query (neither bank_info?/bank_set?) whilst doing bankswitch or condition
                (qry && diskunavail(ctm)) );

    // disk_state?, bank_set? and bank_info? are available in non-bank mode.
    if( qry ) {
        // All replies start with 'nb' as active bank
        reply << " 0 : nb";
        if( args[0]=="bank_set" ) {
            // HV/BE: 8/Nov/2016:
            // Correct non-bank behaviour of "bank_set?" is to reply with
            // both VSNs from the current user directory, if anything
            // recorded in there.
            string              vsn( rte.xlrdev.get_vsn() ), companion( rte.xlrdev.get_companion() );

            // If vsn is empty this means the userdirectory on the 'non-bank
            // pack' did not store its own VSN. In that case we return the
            // current label?
            // NOTE: in theory "reset=erase" will make sure that you can't
            //       create a non-bank disk pack w/o it storing both VSNs.
            //       But it is all too easy to run jive5ab in non-bank mode
            //       with a disk pack with any old 'OriginalLayout' user
            //       directory on it ...
            if( vsn.empty() )
                vsn = rte.xlrdev.read_label();

            vsn       = disk_states::split_vsn_state(vsn).first;
            companion = disk_states::split_vsn_state(companion).first;

            reply << " : " << vsn << " : nb : " << companion;
        }
        else if( args[0]=="disk_state" ) {
            // for disk_state, add some extra info
            // Get the label and extract the vsn state from it
            char          label[XLR_LABEL_LENGTH + 1];

            XLRCODE(SSHANDLE ss = rte.xlrdev.sshandle());
            label[XLR_LABEL_LENGTH] = '\0';

            XLRCALL( ::XLRGetLabel( ss, label) );
            pair<string, string> vsn_state = disk_states::split_vsn_state(label);
            reply << " : " << vsn_state.second;
        }
        reply << " ;";
        return reply.str();
    }
    // bank related commands solicit an error code 6 because we're not in bank mode
    reply << "6 : not in bank mode ;";
    return reply.str();
}

// This one uses the bankinfoset_fn, in case of query
string disk_state_fn( bool qry, const vector<string>& args, runtime& rte) {
    if ( qry ) {
        // forward it to bankinfo_fn
        return bankinfoset_fn( qry, args, rte );
    }

    // handle the command
    ostringstream reply;
    reply << "!" << args[0] << ((qry)?('?'):('='));
    
    if ( rte.transfermode != no_transfer ) {
        reply << " 6 : cannot set state while doing " << rte.transfermode << " ;";
        return reply.str();
    }

    if ( args.size() < 2 ) {
        reply << " 8 : command requires an argument ;";
        return reply.str();
    }

    if ( rte.protected_count == 0 ) {
        reply << " 6 : need an immediate preceding protect=off ;";
        return reply.str();
    }
    // ok, we're allowed to write state

    if ( args[1].empty() ) {
        reply << " 8 : argument is empty ;";
        return reply.str();
    }

    string new_state = args[1];
    new_state[0] = ::toupper(new_state[0]);

    if ( disk_states::all_set.find(new_state) == disk_states::all_set.end() ) {
        reply << " 8 : unrecognized disk state ;";
        return reply.str();
    }

    rte.xlrdev.write_state( new_state );
    
    reply << " 0 : " << new_state << " ;";
    return reply.str();
}

#endif
