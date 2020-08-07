#include "TypedReduction.h"
#include <stdlib.h>

/*readonly*/ CProxy_Driver driverProxy;
/*readonly*/ int array_size;

///////////////////////////////////

static bool approx_equal(double lhs, double rhs)
{
    if (lhs < rhs - 0.5)
        return false;
    if (lhs > rhs + 0.5)
        return false;
    return true;
}

static int fudged_random(int seed_val)
{
    return (seed_val + 101) * 12345 % 67;
}

////////////////////////////////////

Driver::Driver(CkArgMsg* args) {
    driverProxy = thisProxy;
    array_size = 10 * CkNumPes();
    if (args->argc > 1) array_size = strtol(args->argv[1], NULL, 10);
    w = CProxy_Worker::ckNew(array_size);
    w.untyped_reduce();
    delete args;
}

void Driver::untyped_done(CkReductionMsg* m) {
    int* output = (int*)m->getData();
    CkPrintf("Untyped Sum: %d\n", output[0]);
    CkAssert(output[0] == array_size);

    delete m;
    w.typed_reduce();
}

void Driver::typed_done(int result)
{
    CkPrintf("Typed Sum: %d\n", result);
    CkAssert(result == array_size);
    w.reduce_array_ints();
}

void Driver::typed_array_ints_done(int* results, int n)
{
    CkPrintf("Typed Sum: [ ");
    for (int i=0; i<n; ++i) CkPrintf("%d ", results[i]);
    CkPrintf("]\n");

    int contribution[3]={1,2,3};
    for (int i=0; i<n; ++i) CkAssert(results[i] == contribution[i]*array_size);

    w.reduce_indiv_ints();
}

void Driver::typed_indiv_ints_done(int x, int y, int z)
{
    CkPrintf("Typed Sum: (x, y, z) = (%d, %d, %d)\n", x, y, z);

    CkAssert(x == array_size * 1);
    CkAssert(y == array_size * 2);
    CkAssert(z == array_size * 3);

    w.reduce_array_doubles();
}

void Driver::typed_array_doubles_done(int n, double* results)
{
    CkPrintf("Typed Sum: [ ");
    for (int i=0; i<n; ++i) CkPrintf("%.5g ", results[i]);
    CkPrintf("]\n");

    double contribution[3] = { 0.16180, 0.27182, 0.31415 };
    for (int i=0; i<n; ++i) CkAssert(approx_equal(results[i], contribution[i]*array_size));

    w.reduce_set();
}

void Driver::set_done(CkReductionMsg* msg)
{
    CkPrintf("Set Reduction: ");
    size_t set_size = 0;
    size_t total_set_elements = 0;
    CkReduction::setElement* set_result = (CkReduction::setElement*)msg->getData();
    while (set_result && set_result->dataSize > 0)
    {
        size_t numElements = set_result->dataSize / sizeof(int);
        for (size_t idx =0; idx < numElements; ++idx)
            CkPrintf("%d", ((int*)set_result->data)[idx]);
        CkPrintf(", ");
        set_result = set_result->next();
        ++set_size;
        total_set_elements += numElements;
    }
    CkPrintf("\n");

    CkAssert(set_size == array_size);
    int gauss = array_size * (array_size+1) / 2;
    CkAssert(total_set_elements == gauss);
    delete msg;

    CkPrintf("Random data for tuple & statistics reduction: ");
    w.reduce_tuple();
}

void Driver::tuple_reducer_done(CkReductionMsg* msg)
{
    CkPrintf("\n");
    CkReduction::tupleElement* results = NULL;
    int num_reductions = 0;
    msg->toTuple(&results, &num_reductions);

    int min_result = *(int*)results[0].data;
    int max_result = *(int*)results[1].data;
    int sum_result = *(int*)results[2].data;
    CkReduction::statisticsElement& stats_result = *(CkReduction::statisticsElement*)results[3].data;
    CkPrintf("Tuple Reduction: Min: %d, Max: %d, Sum: %d\n",
             min_result, max_result, sum_result);

    CkAssert(max_result >= min_result);
    CkAssert(sum_result >= max_result);

    CkPrintf("Tuple Statistics Reduction: Count: %d, Mean: %.2f, Standard Deviation: %.2f\n",
             stats_result.count, stats_result.mean, stats_result.stddev());

    CkAssert(stats_result.count == array_size);
    CkAssert(stats_result.mean >= min_result);
    CkAssert(stats_result.mean <= max_result);
    CkAssert(approx_equal(stats_result.mean, double(sum_result) / double(array_size)));

    // verify mean & stddev
    {
        int sum = 0;
        for (int i=0; i<array_size; ++i) sum += fudged_random(i);
        double mean = double(sum) / double(array_size);
        CkAssert(approx_equal(stats_result.mean, mean));

        int sum2 = 0;
        for (int i=0; i<array_size; ++i) sum2 += ( fudged_random(i) - mean ) * ( fudged_random(i) - mean );
        double variance = sum2 / double(array_size - 1);
        double stddev = sqrt(variance);
        //CkPrintf("Verification vals %f %f %f\n", mean, variance, stddev);
        CkAssert(approx_equal(stats_result.stddev(), stddev));
    }

    CkPrintf("Tuple Set Reduction: ");
    size_t set_size = 0;
    size_t total_set_elements = 0;
    CkReduction::setElement* set_result = (CkReduction::setElement*)results[4].data;
    while (set_result && set_result->dataSize > 0)
    {
        size_t numElements = set_result->dataSize / sizeof(int);
        for (size_t idx =0; idx < numElements; ++idx)
            CkPrintf("%d", ((int*)set_result->data)[idx]);
        CkPrintf(", ");
        set_result = set_result->next();
        ++set_size;
        total_set_elements += numElements;
    }
    CkPrintf("\n");

    CkAssert(set_size == array_size);
    int gauss = array_size * (array_size+1) / 2;
    CkAssert(total_set_elements == gauss);

    delete[] results;
    delete msg;
    CkExit();
}

////////////////////////////////////////////////

Worker::Worker() { }

void Worker::untyped_reduce() {
    int contribution=1;
    contribute(1*sizeof(int), &contribution, CkReduction::sum_int,
               CkCallback(CkIndex_Driver::untyped_done(NULL), driverProxy));
}

void Worker::typed_reduce() {
    int contribution=1;
    contribute(1*sizeof(int), &contribution, CkReduction::sum_int,
               CkCallback(CkReductionTarget(Driver, typed_done), driverProxy));
}

void Worker::reduce_array_ints() {
    int contribution[3]={1,2,3};
    contribute(3*sizeof(int), contribution, CkReduction::sum_int,
               CkCallback(CkReductionTarget(Driver, typed_array_ints_done), driverProxy));
}

void Worker::reduce_indiv_ints() {
    int contribution[3]={1,2,3};
    contribute(3*sizeof(int), contribution, CkReduction::sum_int,
               CkCallback(CkReductionTarget(Driver, typed_indiv_ints_done), driverProxy));
}

void Worker::reduce_array_doubles() {
    double contribution[3] = { 0.16180, 0.27182, 0.31415 };
    contribute(3*sizeof(double), contribution, CkReduction::sum_double,
               CkCallback(CkReductionTarget(Driver, typed_array_doubles_done), driverProxy));
}

void Worker::reduce_set() {
    int arraySize = thisIndex + 1;
    std::vector<int> setData(arraySize);
    for (int idx = 0; idx < setData.size(); ++idx)
        setData[idx] = idx;

    contribute(setData.size() * sizeof(int), setData.data(), CkReduction::set,
               CkCallback(CkIndex_Driver::set_done(NULL), driverProxy));
}

void Worker::reduce_tuple() {
    // deterministic fudged random numbers - defined above
    int val = fudged_random(thisIndex);
    CkPrintf("%d, ", val);
    CkReduction::statisticsElement stats(val);

    int arraySize = thisIndex + 1;
    std::vector<int> setData(arraySize);
    for (int idx = 0; idx < setData.size(); ++idx)
        setData[idx] = idx;

    int tuple_size = 5;
    CkReduction::tupleElement tuple_reduction[] = {
        CkReduction::tupleElement(sizeof(int), &val, CkReduction::min_int),
        CkReduction::tupleElement(sizeof(int), &val, CkReduction::max_int),
        CkReduction::tupleElement(sizeof(int), &val, CkReduction::sum_int),
        CkReduction::tupleElement(sizeof(CkReduction::statisticsElement), &stats, CkReduction::statistics),
        CkReduction::tupleElement(setData.size() * sizeof(int), setData.data(), CkReduction::set)};

    CkReductionMsg* msg = CkReductionMsg::buildFromTuple(tuple_reduction, tuple_size);

    CkCallback cb(CkIndex_Driver::tuple_reducer_done(NULL), driverProxy);
    msg->setCallback(cb);
    contribute(msg);
}

#include "TypedReduction.def.h"
