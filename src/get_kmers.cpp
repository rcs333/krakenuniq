/*
 * Copyright 2013-2015, Derrick Wood <dwood@cs.jhu.edu>
 *
 * This file is part of the Kraken taxonomic sequence classification system.
 *
 * Kraken is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Kraken is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Kraken.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "kraken_headers.hpp"
#include "quickfile.hpp"
#include "krakendb.hpp"
#include "krakenutil.hpp"
#include "seqreader.hpp"
#include <unordered_map>

#define SKIP_LEN 50000

using namespace std;
using namespace kraken;

void parse_command_line(int argc, char **argv);
void usage(int exit_code=EX_USAGE);
void process_files();
void process_single_file();
void process_file(string filename, uint32_t taxid);
void get_kmers(uint32_t taxid, string &seq, size_t start, size_t finish);

int Num_threads = 1;
string DB_filename, Index_filename, Nodes_filename,
  File_to_taxon_map_filename,
  ID_to_taxon_map_filename, Multi_fasta_filename;
bool force_taxid = false;

bool Allow_extra_kmers = false;
bool verbose = false;
bool Operate_in_RAM = false;
bool One_FASTA_file = false;
map<uint32_t, uint32_t> Parent_map;
map<string, uint32_t> ID_to_taxon_map;
set<uint32_t> All_taxon_ids;
unordered_multimap<uint64_t, uint32_t> Kmer_taxa_map;
map<pair<uint32_t, uint32_t>, uint32_t > TaxidPair_counts;
KrakenDB Database;

int main(int argc, char **argv) {
  #ifdef _OPENMP
  omp_set_num_threads(1);
  #endif

  parse_command_line(argc, argv);

  if (!force_taxid) {
    Parent_map = build_parent_map(Nodes_filename);
  }

  QuickFile db_file(DB_filename, "rw");

  char *temp_ptr = NULL;
  size_t db_file_size = db_file.size();
  if (Operate_in_RAM) {
    cerr << "Getting " << DB_filename << " into memory ... ";
    db_file.close_file();
    temp_ptr = new char[ db_file_size ];
    ifstream ifs(DB_filename.c_str(), ifstream::binary);
    ifs.read(temp_ptr, db_file_size);
    ifs.close();
    Database = KrakenDB(temp_ptr);
    cerr << "done" << endl;
  } else {
    Database = KrakenDB(db_file.ptr());
  }

  KmerScanner::set_k(Database.get_k());

  QuickFile idx_file(Index_filename);
  KrakenDBIndex db_index(idx_file.ptr());
  Database.set_index(&db_index);

  if (One_FASTA_file)
    process_single_file();
  else
    process_files();



  if (Operate_in_RAM) {
    ofstream ofs(DB_filename.c_str(), ofstream::binary);
    ofs.write(temp_ptr, db_file_size);
    ofs.close();
    delete temp_ptr;
  }

  return 0;
}

void process_single_file() {
  cerr << "Processing multiple FASTA files" << endl;
  ifstream map_file(ID_to_taxon_map_filename.c_str());
  if (map_file.rdstate() & ifstream::failbit) {
    err(EX_NOINPUT, "can't open %s", ID_to_taxon_map_filename.c_str());
  }
  string line;
  while (map_file.good()) {
    getline(map_file, line);
    if (line.empty())
      break;
    string seq_id;
    uint32_t taxid;
    istringstream iss(line);
    iss >> seq_id;
    iss >> taxid;
    ID_to_taxon_map[seq_id] = taxid;
  }

  FastaReader reader(Multi_fasta_filename);
  DNASequence dna;
  uint32_t seqs_processed = 0;
  uint32_t seqs_skipped = 0;
  uint32_t seqs_no_taxid = 0;

  while (reader.is_valid()) {
    dna = reader.next_sequence();
    if (! reader.is_valid())
      break;

    if ( dna.seq.empty() ) {
      ++seqs_skipped;
      continue;
    }

    // Get the taxid. If the header specifies kraken:taxid, use that
    uint32_t taxid;
    string prefix = "kraken:taxid|";
    if (dna.id.substr(0,prefix.size()) == prefix) {
        taxid = std::atoi(dna.id.substr(prefix.size()).c_str());
    } else {
        taxid = ID_to_taxon_map[dna.id];
    }

    if (taxid) {

#ifdef _OPENMP
      #pragma omp parallel for schedule(dynamic)
#endif
      for (size_t i = 0; i < dna.seq.size(); i += SKIP_LEN)
        get_kmers(taxid, dna.seq, i, i + SKIP_LEN + Database.get_k() - 1);

        ++seqs_processed;
    } else {
        if (verbose) 
            cerr << "Skipping sequence with header [" << dna.header_line << "] - no taxid" << endl;

        ++seqs_no_taxid;
    }
    cerr << "\rProcessed " << seqs_processed << " sequences";
  }
  cerr << "\r                                                                            ";
  cerr << "\rFinished processing " << seqs_processed << " sequences (skipping "<< seqs_skipped <<" empty sequences, and " << seqs_no_taxid<<" sequences with no taxonomy mapping)" << endl;
}

void process_files() {
  cerr << "Processing files in " << File_to_taxon_map_filename.c_str() << endl;
  ifstream map_file(File_to_taxon_map_filename.c_str());
  if (map_file.rdstate() & ifstream::failbit) {
    err(EX_NOINPUT, "can't open %s", File_to_taxon_map_filename.c_str());
  }
  string line;
  uint32_t seqs_processed = 0;

  while (map_file.good()) {
    getline(map_file, line);
    if (line.empty())
      break;
    string filename;
    uint32_t taxid;
    istringstream iss(line);
    iss >> filename;
    iss >> taxid;
    process_file(filename, taxid);
    cerr << "\rProcessed " << ++seqs_processed << " sequences";
  }
  cerr << "\r                                                       ";
  cerr << "\rFinished processing " << seqs_processed << " sequences" << endl;
}

void process_file(string filename, uint32_t taxid) {
  FastaReader reader(filename);
  DNASequence dna;
  
  // For the purposes of this program, we assume these files are
  // single-fasta files.
  dna = reader.next_sequence();

#ifdef _OPENMP
  #pragma omp parallel for schedule(dynamic)
#endif
  for (size_t i = 0; i < dna.seq.size(); i += SKIP_LEN)
    get_kmers(taxid, dna.seq, i, i + SKIP_LEN + Database.get_k() - 1);
}

void get_kmers(uint32_t taxid, string &seq, size_t start, size_t finish) {

  All_taxon_ids.insert(taxid);
  KmerScanner scanner(seq, start, finish);
  uint64_t *kmer_ptr;

  while ((kmer_ptr = scanner.next_kmer()) != NULL) {
    if (scanner.ambig_kmer())
      continue;

    Kmer_taxa_map.insert({*kmer_ptr, taxid});
  }
}

void parse_command_line(int argc, char **argv) {
  int opt;
  long long sig;

  if (argc > 1 && strcmp(argv[1], "-h") == 0)
    usage(0);
  while ((opt = getopt(argc, argv, "f:d:i:t:n:m:F:xMTv")) != -1) {
    switch (opt) {
      case 'f' :
        File_to_taxon_map_filename = optarg;
        break;
      case 'd' :
        DB_filename = optarg;
        break;
      case 'i' :
        Index_filename = optarg;
        break;
      case 'F' :
        Multi_fasta_filename = optarg;
        break;
      case 'm' :
        ID_to_taxon_map_filename = optarg;
        break;
      case 't' :
        sig = atoll(optarg);
        if (sig <= 0)
          errx(EX_USAGE, "can't use nonpositive thread count");
        #ifdef _OPENMP
        if (sig > omp_get_num_procs())
          errx(EX_USAGE, "thread count exceeds number of processors");
        Num_threads = sig;
        omp_set_num_threads(Num_threads);
        #endif
        break;
      case 'T' :
        force_taxid = true;
        break;
      case 'n' :
        Nodes_filename = optarg;
        break;
      case 'v' :
        verbose = true;
        break;
      case 'x' :
        Allow_extra_kmers = true;
        break;
      case 'M' :
        Operate_in_RAM = true;
        break;
      default:
        usage();
        break;
    }
  }

  if (DB_filename.empty() || Index_filename.empty() ||
      Nodes_filename.empty())
    usage();
  if (File_to_taxon_map_filename.empty() &&
      (Multi_fasta_filename.empty() || ID_to_taxon_map_filename.empty()))
    usage();

  if (! File_to_taxon_map_filename.empty())
    One_FASTA_file = false;
  else
    One_FASTA_file = true;
}

void usage(int exit_code) {
  cerr << "Usage: get_kmers [options]" << endl
       << endl
       << "Options: (*mandatory)" << endl
       << "* -d filename      Kraken DB filename" << endl
       << "* -i filename      Kraken DB index filename" << endl
       << "* -n filename      NCBI Taxonomy nodes file" << endl
       << "  -t #             Number of threads" << endl
       << "  -M               Copy DB to RAM during operation" << endl
       << "  -x               K-mers not found in DB do not cause errors" << endl
       << "  -f filename      File to taxon map" << endl
       << "  -F filename      Multi-FASTA file with sequence data" << endl
       << "  -m filename      Sequence ID to taxon map" << endl
       << "  -T               Do not set LCA as taxid for kmers, but the taxid of the sequence" << endl
       << "  -v               Verbose output" << endl
       << "  -h               Print this message" << endl
       << endl
       << "-F and -m must be specified together.  If -f is given, "
       << "-F/-m are ignored." << endl;
  exit(exit_code);
}
