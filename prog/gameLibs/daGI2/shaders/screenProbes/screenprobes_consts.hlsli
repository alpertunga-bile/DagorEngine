#define SP_INVALID_ADDITIONAL_PROBE ~0u

#define SP_MAX_ADDITIONAL_PROBES_BITS 3
#define SP_MAX_ADDITIONAL_PROBES_MASC ((1<<SP_MAX_ADDITIONAL_PROBES_BITS)-1)
#define SP_MAX_ADDITIONAL_PROBES_COUNT 8 // not more than 1<<SP_MAX_ADDITIONAL_PROBES_BITS
#define SP_ADDITIONAL_ITERATION_INDEX_SHIFT (32-SP_MAX_ADDITIONAL_PROBES_BITS)
#define SP_ADDITIONAL_PROBE_INDEX_MASK ((1<<SP_ADDITIONAL_ITERATION_INDEX_SHIFT) - 1)