# Owner(s): ["oncall: distributed"]

import copy

import torch.distributed._shard.sharded_tensor as sharded_tensor

from torch.distributed._shard.sharding_spec import (
    ChunkShardingSpec,
)
from torch.testing._internal.common_distributed import (
    requires_nccl,
    skip_if_lt_x_gpu,
)

from torch.testing._internal.distributed._shard.sharded_tensor import (
    TEST_GPU_NUM,
    ShardedTensorTestBase,
    with_comms,
)
from torch.testing._internal.common_utils import (
    run_tests,
)

class TestTensorOps(ShardedTensorTestBase):
    @with_comms(init_rpc=False)
    @skip_if_lt_x_gpu(TEST_GPU_NUM)
    @requires_nccl()
    def test_deep_copy(self):
        spec = ChunkShardingSpec(
            dim=0,
            placements=[
                "rank:0/cuda:0",
                "rank:1/cuda:1",
                "rank:2/cuda:2",
                "rank:3/cuda:3",
            ],
        )
        st = sharded_tensor.rand(spec, (12, 5))
        copied_st = copy.deepcopy(st)
        self.assertTrue(type(copied_st) is type(st))
        self.assertEqual(copied_st.local_tensor(), st.local_tensor())
        self.assertFalse(copied_st is st)

if __name__ == "__main__":
    run_tests()
