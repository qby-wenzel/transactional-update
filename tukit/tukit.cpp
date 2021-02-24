/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SPDX-FileCopyrightText: 2020 SUSE LLC */

/*
  transactional-update - apply updates to the system in an atomic way
 */

#include "tukit.hpp"
#include "Configuration.hpp"
#include "Transaction.hpp"
#include "Log.hpp"
#include <fcntl.h>
#include <getopt.h>
#include <unistd.h>
#include <csignal>
#include <cstring>
#include <iostream>

using namespace std;
using TransactionalUpdate::config;

bool cancel;

void TUKit::displayHelp() {
    cout << "Syntax: tukit [option...] command\n";
    cout << "\n";
    cout << "Manage transactions ...\n";
    cout << "\n";
    cout << "Commands:\n";
    cout << "execute <command>\n";
    cout << "\tOpens a new snapshot and executes the given command; on success the snapshot\n";
    cout << "\twill be set as the new default snapshot, any non-zero return value will\n";
    cout << "\tdelete the snapshot again.\n";
    cout << "\tIf no command is given an interactive shell will be opened.\n";
    cout << "open\n";
    cout << "\tCreates a new transaction and prints its unique ID\n";
    cout << "call <ID> <command>\n";
    cout << "\tExecutes the given command from within the transaction's chroot environment,\n";
    cout << "\tresuming the transaction with the given ID; returns the exit status of the\n";
    cout << "\tgiven command, but will not delete the snapshot in case of errors\n";
    cout << "callext <ID> <command>\n";
    cout << "\tExecutes the given command. The command is not executed in a chroot\n";
    cout << "\tenvironment, but instead runs in the current system, replacing '{}' with the\n";
    cout << "\tmount directory of the given snapshot; returns the exit status of the given\n";
    cout << "\tcommand, but will not delete the snapshot in case of errors\n";
    cout << "close <ID>\n";
    cout << "\tCloses the given transaction and sets the snapshot as the new default snapshot\n";
    cout << "abort <ID>\n";
    cout << "\tDeletes the given snapshot again\n";
    cout << "Options:\n";
    cout << "--continue[=<ID>], -c[<ID>]  Use latest or given snapshot as base\n";
    cout << "--help, -h                   Display this help and exit\n";
    cout << "--quiet, -q                  Decrease verbosity\n";
    cout << "--verbose, -v                Increase verbosity\n";
    cout << "--version, -V                Display version and exit\n" << endl;
}

int TUKit::parseOptions(int argc, char *argv[]) {
    static const char optstring[] = "+c::hqv";
    static const struct option longopts[] = {
        { "continue", optional_argument, nullptr, 'c' },
        { "help", no_argument, nullptr, 'h' },
        { "quiet", no_argument, nullptr, 'q' },
        { "verbose", no_argument, nullptr, 'v' },
        { "version", no_argument, nullptr, 'V' },
        { 0, 0, 0, 0 }
    };

    int c;
    int lopt_idx;

    while ((c = getopt_long(argc, argv, optstring, longopts, &lopt_idx)) != -1) {
        switch (c) {
        case 'c':
            if (optarg)
                baseSnapshot = optarg;
            else
                baseSnapshot = "default";
            break;
        case 'h':
            displayHelp();
            return 0;
        case 'q':
            tulog.level = TULogLevel::ERROR;
            break;
        case 'v':
            tulog.level = TULogLevel::DEBUG;
            break;
        case 'V':
            cout << VERSION << endl;
            return 0;
        case '?':
            displayHelp();
            return -1;
        }
    }

    return optind;
}

int TUKit::processCommand(char *argv[]) {
    TransactionalUpdate::Transaction transaction{};

    if (argv[0] == nullptr) {
        throw invalid_argument{"Missing command. See --help for usage information."};
    }
    string arg = argv[0];
    if (arg == "execute") {
        transaction.init(baseSnapshot);
        int status = transaction.execute(&argv[1]); // All remaining arguments
        if (status == 0) {
            transaction.finalize();
        } else {
            throw runtime_error{"Application returned with exit status " + to_string(status)};
        }
        return 0;
    }
    else if (arg == "open") {
        transaction.init(baseSnapshot);
        cout << "ID: " << transaction.getSnapshot() << endl;
        transaction.keep();
        return 0;
    }
    else if (arg == "call") {
        if (argv[1] == nullptr) {
            displayHelp();
            throw invalid_argument{"Missing argument for 'call'"};
        }
        transaction.resume(argv[1]);
        int status = transaction.execute(&argv[2]); // All remaining arguments
        transaction.keep();
        return status;
    }
    else if (arg == "callext") {
        if (argv[1] == nullptr) {
            displayHelp();
            throw invalid_argument{"Missing argument for 'callext'"};
        }
        transaction.resume(argv[1]);
        int status = transaction.callExt(&argv[2]); // All remaining arguments
        transaction.keep();
        return status;
    }
    else if (arg == "close") {
        transaction.resume(argv[1]);
        transaction.finalize();
        return 0;
    }
    else if (arg == "abort") {
        transaction.resume(argv[1]);
        return 0;
    }
    else {
        displayHelp();
        throw invalid_argument{"Unknown command or option '" + arg + "'."};
    }
}

class Lock {
public:
    Lock(){
        lockfile = open(config.get("LOCKFILE").c_str(), O_CREAT|O_WRONLY, 0600);
        if (lockfile < 0) {
            throw runtime_error{"Could not create lock file '" + config.get("LOCKFILE") + "': " + strerror(errno)};
        }
        int status = lockf(lockfile, F_TLOCK, (off_t)10000);
        if (status) {
            throw runtime_error{"Another instance of tukit is already running: " + string(strerror(errno))};
            remove(config.get("LOCKFILE").c_str());
        }
    }
    ~Lock() {
        close(lockfile);
        remove(config.get("LOCKFILE").c_str());
    }
private:
    int lockfile;
};

void interrupt(int signal) {
    //Nothing to do here - the child has been signalled already as it's part of the same
    // progress group. Maybe it may be worth killing the process when receiving multiple
    // interrupts?
    tulog.debug("tukit: Received signal ", signal);
}

TUKit::TUKit(int argc, char *argv[]) {
    signal(SIGINT, interrupt);
    signal(SIGHUP, interrupt);
    signal(SIGQUIT, interrupt);
    signal(SIGTERM, interrupt);

    tulog.level = TULogLevel::INFO;

    int ret = parseOptions(argc, argv);
    if (ret <= 0) {
        throw ret;
    }

    Lock lock;
    tulog.info("tukit ", VERSION, " started");

    string optionsline = "Options: ";
    for(int i = 1; i < argc; ++i)
        optionsline.append(argv[i]).append(" ");
    tulog.info(optionsline);

    ret = processCommand(&argv[ret]);
    if (ret != 0) {
        throw ret;
    }

    tulog.info("Transaction completed.");
}
