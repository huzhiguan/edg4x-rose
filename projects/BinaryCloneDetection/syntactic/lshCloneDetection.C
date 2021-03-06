#include <sys/time.h>
#include <sys/resource.h>

#include "findExactClones.h"
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/variables_map.hpp>

using namespace boost::program_options;
using boost::lexical_cast;

using namespace std;
inline double tvToDouble(const timeval& tv) {
    return tv.tv_sec + tv.tv_usec * 1.e-6;
}

void
insert_timing(const SqlDatabase::TransactionPtr &tx, std::string property_name, const int groupLow, const int groupHigh,
              const int num_elements, const int k, const int l, const timeval& before, const timeval& after,
              const rusage& ru_before, const rusage& ru_after)
{
    SqlDatabase::StatementPtr cmd = tx->statement("insert into group_timing"
                                                  // 0         1          2             3  4  5
                                                  " (groupLow, groupHigh, num_elements, K, L, total_wallclock,"
                                                  // 6              7              8          9         10
                                                  " total_usertime, total_systime, wallclock, usertime, systime)"
                                                  " values (?,?,?,?,?,?,?,?,?,?,?)");
    cmd->bind(0, groupLow);
    cmd->bind(1, groupHigh);
    cmd->bind(2, num_elements);
    cmd->bind(3, k);
    cmd->bind(4, l);
    cmd->bind(5, 0);
    cmd->bind(6, tvToDouble(ru_after.ru_utime));
    cmd->bind(7, tvToDouble(ru_after.ru_stime));
    cmd->bind(8, (tvToDouble(after) - tvToDouble(before)));
    cmd->bind(9, (tvToDouble(ru_after.ru_utime) - tvToDouble(ru_before.ru_utime)));
    cmd->bind(10, (tvToDouble(ru_after.ru_stime) - tvToDouble(ru_before.ru_stime)));
    cmd->execute();
}

bool
compare_rows(const VectorEntry* lhs1, const VectorEntry* rhs1)
{
    return lhs1->rowNumber < rhs1->rowNumber;
}


void
read_vector_data(const SqlDatabase::TransactionPtr &tx, scoped_array_with_size<VectorEntry>& vectors, const size_t& groupLow,
                 const int& groupHigh)
{
    size_t eltCount = 0;
    SqlDatabase::StatementPtr cmd1 = tx->statement(groupHigh == -1
                                                   ? "select count(*) from vectors where sum_of_counts >= ?"
                                                   : "select count(*) from vectors where sum_of_counts >= ?"
                                                   " and sum_of_counts <= ?");
    cmd1->bind(0, groupLow);
    if (groupHigh != -1)
        cmd1->bind(1, groupHigh);
    eltCount = cmd1->execute_int();

    if (eltCount == 0) {
        cerr << "No vectors found -- invalid database?" << endl;
        exit (1);
    }

    if (debug_messages)
        cout << "Found " << eltCount << " vectors" << endl;

    vectors.allocate(eltCount);
    SqlDatabase::StatementPtr cmd2 = tx->statement(groupHigh == -1
                                                   ? "select id, function_id, index_within_function, line, offset,"
                                                   " counts_b64, instr_seq_b64 from vectors where sum_of_counts >= ?"
                                                   : "select id, function_id, index_within_function, line, offset,"
                                                   " counts_b64, instr_seq_b64 from vectors where sum_of_counts >= ?"
                                                   " and sum_of_counts <= ?");
    cmd2->bind(0, groupLow);
    if (groupHigh != -1)
        cmd2->bind(1, groupHigh);
    size_t indexInVectors=0;
    for (SqlDatabase::Statement::iterator r=cmd2->begin(); r!=cmd2->end(); ++r) {
        long long rowNumber = r.get<int64_t>(0);
        int functionId = r.get<int>(1);
        int indexWithinFunction = r.get<int>(2);
        long long line = r.get<int64_t>(3);
        int offset = r.get<int>(4);
        std::vector<uint8_t> counts = StringUtility::decode_base64(r.get<std::string>(5));
        string compressedCounts(&counts[0], &counts[0]+counts.size());
        std::vector<uint8_t> md5 = StringUtility::decode_base64(r.get<std::string>(6));
        string instrSeqMD5(&md5[0], &md5[0]+md5.size());
        VectorEntry& ve = vectors[indexInVectors];
        ve.functionId = functionId;
        ve.indexWithinFunction = indexWithinFunction;
        ve.line = line;
        ve.offset = offset;
        ve.compressedCounts.allocate(compressedCounts.size());
        ve.rowNumber = rowNumber;
        memcpy(ve.compressedCounts.get(), compressedCounts.data(), compressedCounts.size());
        if (instrSeqMD5.size() != 16) {
            if (debug_messages)
                cout << "Found MD5 with length other than 16" << endl;
            abort();
        }
        memcpy(ve.instrSeqMD5, instrSeqMD5.data(), 16);
        if (rowNumber % 100000 == 0)
            cerr << "Got row " << rowNumber << endl;
        indexInVectors++;
    }
}

int
main(int argc, char* argv[])
{
    std::string database;
    size_t l = 4, k = 700;
    size_t hashTableNumBuckets = 13000000, hashTableElementsPerBucket = 20;
    double distBound = 1.;
    double similarity=1.;
    double r = 4.;
    int norm = 1;
    int groupLow=-1;
    int groupHigh=-1;

    //Timing
    struct timeval before, after;
    struct rusage ru_before, ru_after;
    gettimeofday(&before, NULL);
    getrusage(RUSAGE_SELF, &ru_before);

    bool nodelete = false;
    try {
        options_description desc("Allowed options");
        desc.add_options()
            ("help", "Produce a help message")
            ("nodelete", "Do not delete from vectors")
            ("groupLow,g", value< int >(&groupLow), "The lowest count of elements")
            ("groupHigh,G", value< int >(&groupHigh), "The highest count of elements")
            ("database", value< string >(&database), "The sqlite database that we are to use")
            ("hash-function-size,k", value< size_t >(&k), "The number of elements in a single hash function")
            ("hash-table-count,l", value< size_t >(&l), "The number of separate hash tables to create")
            ("buckets,b", value< size_t >(&hashTableNumBuckets),
             "The number of buckets in each hash table (buckets may store multiple elements)")
            ("bucket-size,s", value< size_t >(&hashTableElementsPerBucket),
             "The number of elements that can be stored in each hash table bucket")
            ("similarity,t", value< double >(&similarity), "The similarity threshold that is allowed in a clone pair")
            ("distance,d", value< double >(&distBound), "The maximum distance that is allowed in a clone pair")
            ("interval-size,r", value< double >(&r), "The divisor for the l_2 hash function family")
            ("norm,p", value< int >(&norm), "Exponent in p-norm to use (1 or 2)")
            ;
        variables_map vm;
        store(parse_command_line(argc, argv, desc), vm);
        notify(vm);

        distBound = similarity==1  ? 0.0 : sqrt(2*groupLow*(1.-similarity));

        std::cerr << "similarity " << similarity << " distBound " << distBound << std::endl;

        if (vm.count("help")) {
            cout << desc << endl;
            exit(0);
        }
        if (vm.count("nodelete")) {
            nodelete = true;
        }
        if (vm.count("groupLow") == 0) {
            groupLow = -1;
        }
        if (vm.count("groupHigh") == 0) {
            groupHigh = -1;
        }
        if (database == "") {
            std::cerr << "Missing options. Call as: " << argv[0] << " --database <database-name> [other parameters]" 
                      << std::endl;
            exit(1);
        }
        if (hashTableNumBuckets >= (1ULL << 32)) {
            cerr << "Number of buckets must be less than 2**32" << endl;
            exit (1);
        }
        if (norm != 1 && norm != 2) {
            cerr << "Norm must be either 1 or 2" << endl;
            exit (1);
        }
        if (nodelete == false) {
            cerr << "groupLow: " << groupLow << std::endl;
            cerr << "groupHigh: " << groupHigh << std::endl;
            cerr << "norm: l_" << norm << std::endl;
            cerr << "database: " << database << std::endl;
            cerr << "k: " << k << std::endl;
            cerr << "l: " << l << std::endl;
            cerr << "buckets: " << hashTableNumBuckets << std::endl;
            cerr << "bucket size: " << hashTableElementsPerBucket << std::endl;
            cerr << "distance: " << distBound << std::endl;
            cerr << "r: " << r << std::endl;
        }
    } catch(exception& e) {
        cout << e.what() << "\n";
        exit (1);
    }

    SqlDatabase::TransactionPtr tx = SqlDatabase::Connection::create(database)->transaction();

    scoped_array_with_size<VectorEntry> vectors;
    scoped_array_with_size<scoped_array_with_size<VectorEntry> > duplicateVectors;
  
    //Step to pass to LSH only the vectors that are not part of an exact clone pass
    {
        std::vector<int> functionsThatWeAreInterestedIn;
        scoped_array_with_size<VectorEntry> allVectors;
        map<string, std::vector<int> > internTable;
        read_vector_data(tx, allVectors, functionsThatWeAreInterestedIn, internTable, groupLow, groupHigh, false);

        //Assign to vectors the first element of each hash bucket
        int numberOfBuckets = 0;
        for (map<string, std::vector<int> >::iterator iItr = internTable.begin(); iItr != internTable.end() ; iItr++)
            numberOfBuckets++;

        vectors.allocate(numberOfBuckets);
        duplicateVectors.allocate(numberOfBuckets);

        int indexInVectors=0;
        std::cout << "All is size: " << allVectors.size() << " reduced size is " << vectors.size() << std::endl;
        for (map<string, std::vector<int> >::iterator iItr = internTable.begin(); iItr != internTable.end() ; iItr++) {
            if (iItr->second.size()>1)
                duplicateVectors[indexInVectors].allocate(iItr->second.size()-1);
            for (unsigned int i = 0 ; i < iItr->second.size() ; i++) {
                VectorEntry& allVe = allVectors[iItr->second[i]];
                VectorEntry& ve = i == 0 ? vectors[indexInVectors] : duplicateVectors[indexInVectors][i-1];
                ve.rowNumber  = allVe.rowNumber;
                ve.functionId = allVe.functionId;
                ve.indexWithinFunction = allVe.indexWithinFunction;
                ve.line = allVe.line;
                ve.offset = allVe.offset;
                ve.compressedCounts.allocate(allVe.compressedCounts.size());
                memcpy(ve.compressedCounts.get(), allVe.compressedCounts.get(), allVe.compressedCounts.size());
            }
            indexInVectors++;
        }
    }

    if (vectors[0].compressedCounts.size() == 0) {
        cerr << "Vector slot 0 is empty" << endl;
        abort();
    }

    size_t numVectorElements = getUncompressedSizeOfVector(vectors[0].compressedCounts.get(), vectors[0].compressedCounts.size());
    if (debug_messages) {
        cout << "Vectors have " << numVectorElements << " elements" << endl;
        cout << "Number of vectors fetched is " << vectors.size() << std::endl;
    }
    
    LSHTableBase* table = NULL;
    switch (norm) {
        case 1:
            table = new LSHTable<HammingHashFunctionSet, L1DistanceObject>(vectors, L1DistanceObject(), k, l, r,
                                                                           numVectorElements, hashTableNumBuckets,
                                                                           hashTableElementsPerBucket, distBound);
            break;
        case 2:
            table = new LSHTable<StableDistributionHashFunctionSet, L2DistanceObject>(vectors, L2DistanceObject(), k, l, r,
                                                                                      numVectorElements, hashTableNumBuckets,
                                                                                      hashTableElementsPerBucket, distBound);
            break;
        default:
            cerr << "Bad value for --norm" << endl;
            abort(); // Should have been caught earlier
    }
    assert(table);

    // Setup stuff for postprocessing
    int windowSize = 0;
    int stride = 0;
    get_run_parameters(tx, windowSize, stride);

    if (nodelete == false) {
        cerr << "About to delete from clusters" << endl;
        tx->execute("delete from clusters");
        cerr << "... done" << endl;
        cerr << "About to delete from postprocessed_clusters" << endl;
        tx->execute("delete from postprocessed_clusters");
        cerr << "... done" << endl;
    }
    const size_t numStridesThatMustBeDifferent = windowSize / (stride * 2);

    // Get clusters and postprocess them
    vector<bool> liveVectors(vectors.size(), true);
    size_t clusterNum = 0, postprocessedClusterNum = 0;
    for (size_t i = 0; i < vectors.size(); ++i) { //Loop over vectors
        //Creating potential clusters
        if (!liveVectors[i])
            continue;
        liveVectors[i] = false;
        vector<pair<size_t, double> > clusterElementsRaw = table->query(i); // Pairs are vector number, distance
        vector<pair<uint64_t, double> > clusterElements;
        vector<uint64_t > postprocessedClusterElements;
        clusterElements.push_back(make_pair(i, 0));

        //const VectorEntry& ve = vectors[i];
        for (size_t j = 0; j < clusterElementsRaw.size(); ++j) {
            size_t entry = clusterElementsRaw[j].first;
            //double dist = clusterElementsRaw[j].second;
            // All entries less than i were in previous clusters, so we save an array lookup
            if (entry <= i || !liveVectors[entry]) continue;
            clusterElements.push_back(clusterElementsRaw[j]);
            liveVectors[entry] = false;
        }
        if (clusterElements.size() < 2 && duplicateVectors[i].size() == 0 )
            continue;

        //Insert raw cluster data 
        for (vector<pair<uint64_t, double> >::const_iterator j = clusterElements.begin(); j != clusterElements.end(); ++j) {
            for(size_t k = 0; k < duplicateVectors[j->first].size(); k++) {
                const VectorEntry& ve = duplicateVectors[j->first][k];
                insert_into_clusters(tx, clusterNum, ve.functionId, ve.indexWithinFunction, ve.rowNumber, j->second);
            }
            const VectorEntry& ve = vectors[j->first];
            insert_into_clusters(tx, clusterNum, ve.functionId, ve.indexWithinFunction, ve.rowNumber, j->second);
        }
        if (clusterNum % 10000 == 0 && debug_messages)
            cerr << "cluster " << clusterNum << " has " << clusterElements.size() << " elements" << endl;
        ++clusterNum;

        //Postprocessing does not make sense for inexact clones
        if (similarity != 1.0 )
            continue;

        // This implicitly groups elements in the same function together and order by index_within_function in each function
        // Not needed because of the sort in LSHTable::query() which is on the cluster number:
        // std::sort(clusterElements.begin(), clusterElements.end());

        //The next two variables will we initialized in first run
        size_t lastFunctionId=0;
        size_t lastIndexWithinFunction=0;
        bool first = true;
        std::vector<VectorEntry*> clusterElemPtr;
        for (size_t j = 0; j < clusterElements.size(); ++j) {
            clusterElemPtr.push_back( &vectors[ clusterElements[j].first ]  );
            for (size_t k = 0; k < duplicateVectors[clusterElements[j].first].size(); k++)
                clusterElemPtr.push_back(&duplicateVectors[ clusterElements[j].first ][k]);
        }

        std::sort(clusterElemPtr.begin(), clusterElemPtr.end(), compare_rows );
        for (size_t j = 0; j < clusterElemPtr.size(); ++j) {
            const VectorEntry& ve = *clusterElemPtr[j];
            if (first || ve.functionId != lastFunctionId ||
                ve.indexWithinFunction >= lastIndexWithinFunction + numStridesThatMustBeDifferent) {
                lastFunctionId = ve.functionId;
                lastIndexWithinFunction = ve.indexWithinFunction;
                postprocessedClusterElements.push_back(j);
            }
            first = false;
        }
        if (postprocessedClusterElements.size() >= 2) { //insert post processed data 
            for (vector<uint64_t >::const_iterator j = postprocessedClusterElements.begin();
                 j != postprocessedClusterElements.end(); ++j) {
                const VectorEntry& ve = *clusterElemPtr[*j];
                insert_into_postprocessed_clusters(tx, postprocessedClusterNum, ve.functionId, ve.indexWithinFunction,
                                                   ve.rowNumber, 0);
            }
            if (postprocessedClusterNum % 1000 == 0) {
                cerr << "postprocessed cluster " << postprocessedClusterNum
                     << " has " << postprocessedClusterElements.size() << " elements" << endl;
            }
            ++postprocessedClusterNum;
        }
    }
    cerr << clusterNum << " total cluster(s), " << postprocessedClusterNum << " after postprocessing" << endl;

    gettimeofday(&after, NULL);
    getrusage(RUSAGE_SELF, &ru_after);
    insert_timing(tx, "lsh", groupLow, groupHigh, vectors.size(), k, l, before,after, ru_before, ru_after);

    tx->commit();
    return 0;
}
