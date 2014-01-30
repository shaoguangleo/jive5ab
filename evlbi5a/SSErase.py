#!/usr/bin/env python

# see http://pypi.python.org/pypi/argparse for installation instructions of argparse (for python 2.6 or lower, 2.7 and up have it by default)
import argparse
import socket
import time
import sys

to_gb = lambda x: float(x)/1.0e9

def split_reply(reply):
    end_index = reply.rfind(';')
    if end_index != -1:
        reply = reply[:end_index]
    separator_index = reply.find('=')
    if separator_index == -1:
        separator_index = reply.find('?')
        if separator_index == -1:
            return [reply]

    return map(lambda x: x.strip(), [reply[0:separator_index]] + reply[separator_index+1:].split(': '))

class Mark5(object):
    def __init__(self, address, port):
        self.connect_point = (address, port)
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.socket.settimeout(5)
        self.socket.connect(self.connect_point)
    
        self.type = self.check_type()
        if self.type not in ["mark5A", "mark5b", "Mark5C"]:
            raise RuntimeError("Failed to recognize Mark5 type '%s'" % self.type)

    def check_type(self):
        return self.send_query("dts_id?")[2]

    def _split_check(self, query, reply, acceptable):
        split = split_reply(reply)
        if split[1] not in acceptable:
            raise RuntimeError("Query ('%s') execution failed, reply: '%s'" % (query, reply)) # all command send in this program require succesful completion
        return split

    def send_query(self, query, acceptable = ["0", "1"]):
        self.socket.send(query + "\n\r")
        reply = self.socket.recv(1024)
        return self._split_check(query, reply, acceptable)

    def send_queries(self, queries, acceptable = ["0", "1"]):
        query = ";".join(queries)
        self.socket.send(query + "\n\r")
        reply = self.socket.recv(1024)
        query_replies = reply.split(";")[:-1] # -1 as we have an "extra ;" at the end of the string
        if len(queries) != len(query_replies): 
            raise RuntimeError("Number of query replies is different from number of queries (send: '%s', received '%s')" % (query, reply))
        return map(lambda (q, r): self._split_check(q, r, acceptable), zip(queries, query_replies))
        
def generate_parser():
    parser = argparse.ArgumentParser(description = "Erase disk(s) mounted in the target machine. Apply conditioning while erasing if requested.")
    
    parser.add_argument("-a", "--address", default = "localhost", help = "Mark5 IP or host name")
    parser.add_argument("-p", "--port", default = 2620, type = int, help = "port to send queries to")
    parser.add_argument("-c", "--condition", default = 0, type = int, help = "apply conditioning to the disk (1: apply, default 0)")
    parser.add_argument("-d", "--debug", default = 0, type = int, help = "print progress of conditioning (1: print, default 0)")
    parser.add_argument("-t", "--debug_time", default = 60, type = int, help = "amount of time between progress updates")
    parser.add_argument("-g", "--gigabyte", action = "store_true", help = "show values DirList in units of GB (10^9 bytes)")
    parser.add_argument("--test", action = "store_true", help = argparse.SUPPRESS)
    
    return parser

def set_bank(mk5, args, bank):
    mk5.send_query("bank_set=%s" % bank)

    start = time.time()
    timeout = 5
    while (time.time() - start) < timeout:
        # bank_set will return 5 or 6 while switching banks (depends on version)
        bank_status = mk5.send_query("bank_set?", ["0", "1", "5", "6"])[1]
        if bank_status == "0":
            break
        time.sleep(0.1)
    if bank_status != "0":
        raise RuntimeError("Bank switching timed out after %ds" % timeout)

def print_dir_list(mk5, args, bank, vsn):
    print "VSN <{vsn}> in bank {bank} contents:".format(vsn = vsn, bank = bank)
    set_bank(mk5, args, bank)
    dir_info = mk5.send_query("dir_info?")
    number_scans = int(dir_info[2])
    record_pointer = int(dir_info[3])
    size = int(dir_info[4])

    if args.gigabyte:
        byte_to_text = lambda byte: "%.09f GB" % to_gb(byte)
    else:
        byte_to_text = lambda byte: "%d B" % byte
    
    if number_scans == 0:
        if record_pointer != 0:
            print "No scans in DirList, but record pointer = {record}".format(record = byte_to_text(record_pointer))
        else:
            print "Disk pack is empty"
        return

    columns = ["exper/station", "start scan", "end scan", "start byte", "end byte"]
    column_alignment = { "exper/station" : "<", 
                         "start scan" : ">", 
                         "end scan" : ">", 
                         "start byte" : ">", 
                         "end byte" : ">" }
    
    # gather scan summary
    previous_valid = False
    scans = [{column : column for column in columns}]
    for scan_index in xrange(number_scans):
        scan_info = mk5.send_queries(["scan_set={scan}".format(scan = scan_index + 1), "scan_set?"])[1]
        scan_name = scan_info[3]
        split_name = scan_name.split("_")
        start_byte = byte_to_text(int(scan_info[4]))
        end_byte = byte_to_text(int(scan_info[5]))
        if len(split_name) == 3:
            exp_station = "/".join(split_name[:2])
            if not previous_valid or (scans[-1]["exper/station"] != exp_station):
                # new experiment/station
                scans.append({"exper/station" : exp_station,
                              "start scan" : split_name[2],
                              "start byte" : start_byte})
            scans[-1]["end scan"] = split_name[2]
            scans[-1]["end byte"] = end_byte
            previous_valid = True
        else:
            scans.append({"exper/station" : scan_name,
                          "start scan" : "",
                          "end scan" : "",
                          "start byte" : start_byte,
                          "end byte" : end_byte})
            previous_valid = False
        
    # print the columns properly aligned
    column_size = { column : max(map(lambda x: len(x[column]), scans)) for column in columns }
    format_string = " | ".join(map(lambda column: ("{%s:%s%d}" % (column, column_alignment[column], column_size[column])), columns))
    for scan in scans:
        print format_string.format(**scan)

    print "Size: {size}  Scans: {scans}  Recorded: {recorded}".format(
        size = byte_to_text(size),
        scans = number_scans,
        recorded = byte_to_text(record_pointer))
    
    # try to find a start / end time
    def get_time(reply, source_field, time_field, invalids):
        if reply[source_field] not in invalids:
            return reply[time_field]
        else:
            return "unknown"
    if mk5.type == "mark5b":
        invalids = ["tvg", "?"]
        source_field = 2
        time_field = 3
    else:
        invalids = ["SS", "tvg", "?"]
        source_field = 2
        time_field = 4
    data_check = mk5.send_queries(["scan_set=1","data_check?"])[1]
    start_time = get_time(data_check, source_field, time_field, invalids)
    data_check = mk5.send_queries(["scan_set={scan}:-1000000".format(scan = number_scans),"data_check?"])[1] # check near the end of the last scan
    end_time = get_time(data_check, source_field, time_field, invalids)
    print "Start time: {start}  End time {end}".format(start = start_time, end = end_time)

def confirm_erase_bank(mk5, args, bank, vsn):
    print
    print_dir_list(mk5, args, bank, vsn)
    print
    sys.stdout.write("Are you sure that you want to erase %s in bank %s ? (Y or N)  " % (vsn, bank))
    continue_reply = sys.stdin.readline()
    return continue_reply[0] in ["Y", "y"]

def get_banks_to_erase(mk5, args):
    """
    Ask the user which banks to erase on mark5 mk5 (of type Mark5).
    Returns a list of banks.
    """
    banks_reply = mk5.send_query("bank_set?")

    if banks_reply[2] == "-": # active bank
        print "Nothing mounted"
        return []

    banks = []
    if confirm_erase_bank(mk5, args, banks_reply[2], banks_reply[3]):
        banks.append(banks_reply[2])
    if (banks_reply[4] != "-") and confirm_erase_bank(mk5, args, banks_reply[4], banks_reply[5]): # inactive bank
        banks.append(banks_reply[4])
    
    return banks

class Erase_Results(object):
    def __init__(self):
        self.duration = None
        self.disk_stats = {}
        self.min_data_rate = None
        self.max_data_rate = None
        self.stat_thresholds = None

def erase(mk5, args, bank):
    """
    Perform an erase of the given bank (A or B) on mk5 (of type Mark5), 
    using arguments args (object with members the arguments given)
    Returns an object with members:
     -duration
     -disk_stats ({(drive_number, serial) : [bin stats]})
     -min_data_rate
     -max_data_rate
     -stat_thresholds
    """

    set_bank(mk5, args, bank)
    
    results = Erase_Results()
    if args.condition:
        results.stat_thresholds = [ 0.001125 * 2**i for i in xrange(7) ]
        mk5.send_query("start_stats=%s" % " : ".join(map(lambda x: "%.6fs" % x, results.stat_thresholds)))
        if args.debug:
            # compute the number of busses such that we can compute the percentage still to go
            master_disks = mk5.send_query("disk_serial?")[2::2]
            number_busses = len(filter(lambda x: len(x) > 0, master_disks))
    
    print "Bank", bank
    dir_info = mk5.send_query("dir_info?")
    pack_size = int(dir_info[4])
    then = time.time()
    if args.condition:
        # do an erase, otherwise the read loop will skip the bytes still on disk (bug in StreamStor)
        mk5.send_queries(["protect=off","reset=erase"])
        mk5.send_queries(["protect=off","reset=condition"])
        try:
            if args.debug:
                prev_time = time.time()
                prev_byte = None
                pass_name = "Read"
            while True:
                transfer = mk5.send_query("tstat=")
                if transfer[3] == "no_transfer":
                    break

                if args.debug:
                    if mk5.type == "mark5A":
                        position = mk5.send_query("position?")
                    else:
                        position = mk5.send_query("pointers?")
                    byte = int(position[2]) * number_busses

                    now = time.time()

                    data_rate_text = ""
                    if prev_byte != None:
                        if byte > prev_byte:
                            pass_name = "Write"
                        else:
                            data_rate = (prev_byte - byte)/(now - prev_time)
                            if (results.min_data_rate == None) or (data_rate < results.min_data_rate):
                                results.min_data_rate = data_rate
                            if (results.max_data_rate == None) or (data_rate > results.max_data_rate):
                                results.max_data_rate = data_rate

                            data_rate_text = " at %.0f Mbps" % (data_rate * 8 / 1000**2)
                    
                    if args.gigabyte:
                        bytes_text = "%13.7f GB" % to_gb(byte)
                    else:
                        bytes_text = "%d B" % byte

                    print "Bank %s %s cycle progress: %s to go (%d%%)%s" % (bank, pass_name, bytes_text, 100*byte/pack_size, data_rate_text)

                    prev_byte = byte
                    prev_time = now
                    time.sleep(args.debug_time)
        except:
            print "Exception during conditioning, trying to abort, exception:", sys.exc_info()[1]
            # try to stop the conditioning
            mk5.send_query("reset=abort")
            raise
    else:
        mk5.send_queries(["protect=off","reset=erase"])

    serials = mk5.send_query("disk_serial?")
    stats = mk5.send_query("get_stats?")
    start_drive = int(stats[2])
    while True:
        drive = int(stats[2])
        results.disk_stats[(drive, serials[drive + 2])] = map(int, stats[3:12])
        stats = mk5.send_query("get_stats?")
        if int(stats[2]) == start_drive:
            break

    results.duration = time.time() - then
    return results

def erase_test(mk5, args, bank):
    """
    Just for debugging purposes
    """
    ret = Erase_Results()
    ret.duration = 2 * 60 * 60
    ret.disk_stats = { (disk, "disk%d" % disk) : range(9) for disk in xrange(8) }
    ret.min_data_rate = 255e6
    ret.max_data_rate = 257e6
    ret.stat_thresholds = range(7)
    return ret
                   
if __name__ == "__main__":
    parser = generate_parser()
    args = parser.parse_args()

    if args.test:
        print "============== WARNING in test mode ==============="
        erase = erase_test

    mk5 = Mark5(args.address, args.port)
    
    banks = get_banks_to_erase(mk5, args)
    if len(banks) == 0:
        print "Nothing to erase"
        sys.exit()
    for bank in banks:
        erase_results = erase(mk5, args, bank)

        for ((drive, serial), stats) in sorted(erase_results.disk_stats.items()):
            print "%d, %s: %s" % (drive, serial, " : ".join(map(str,stats)))
        if args.condition:
            pack_size = int(mk5.send_query("dir_info?")[4])
            print "Conditioning %.1f GB in Bank %s took %d secs ie. %.1f mins" % (pack_size/1000000000, bank, erase_results.duration, (erase_results.duration)/60)
            to_mbps = lambda x: x * 8 / 1000**2
            print "Minimum data rate %.0f Mbps, maximum data rate %.0f Mbps" % (to_mbps(erase_results.min_data_rate), to_mbps(erase_results.max_data_rate))
