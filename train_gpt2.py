"""
Reference code for GPT-2 training and inference.
Will save the model weights into files, to be read from C as initialization.

References:
1) the official GPT-2 TensorFlow implementation released by OpenAI:
https://github.com/openai/gpt-2/blob/master/src/model.py
2) huggingface/transformers PyTorch implementation:
https://github.com/huggingface/transformers/blob/main/src/transformers/models/gpt2/modeling_gpt2.py
"""

import os
import math
import struct
from contextlib import nullcontext
from dataclasses import dataclass

import numpy as np
import torch
import torch.nn as nn
from torch.nn import functional as F
import torch._inductor.config as config

class NewGELU(nn.Module):
    """Careful there are a few versions of GeLU, this one is the exact one used by OpenAI"""
    def forward(self, input):
        return 0.5 * input * (1.0 + torch.tanh(math.sqrt(2.0 / math.pi) * (input + 0.044715 * torch.pow(input, 3.0))))

class CausalSelfAttention(nn.Module):

    def __init__(self, config):
        super().__init__()
        assert config.n_embd % config.n_head == 0
        # key, query, value projections for all heads, but in a batch
        self.c_attn = nn.Linear(config.n_embd, 3 * config.n_embd)
        # output projection
        self.c_proj = nn.Linear(config.n_embd, config.n_embd)
        # regularization
        self.n_head = config.n_head
        self.n_embd = config.n_embd
        # not really a 'bias', more of a mask, but following the OpenAI/HF naming though
        self.register_buffer("bias", torch.tril(torch.ones(config.block_size, config.block_size))
                                     .view(1, 1, config.block_size, config.block_size))

    def forward(self, x):
        B, T, C = x.size() # batch size, sequence length, embedding dimensionality (n_embd)
        # calculate query, key, values for all heads in batch and move head forward to be the batch dim
        qkv = self.c_attn(x)
        q, k, v = qkv.split(self.n_embd, dim=2)
        k = k.view(B, T, self.n_head, C // self.n_head).transpose(1, 2) # (B, nh, T, hs)
        q = q.view(B, T, self.n_head, C // self.n_head).transpose(1, 2) # (B, nh, T, hs)
        v = v.view(B, T, self.n_head, C // self.n_head).transpose(1, 2) # (B, nh, T, hs)
        # manual implementation of attention
        att = (q @ k.transpose(-2, -1)) * (1.0 / math.sqrt(k.size(-1)))
        att = att.masked_fill(self.bias[:,:,:T,:T] == 0, float('-inf'))
        att = F.softmax(att, dim=-1)
        y = att @ v # (B, nh, T, T) x (B, nh, T, hs) -> (B, nh, T, hs)
        y = y.transpose(1, 2).contiguous().view(B, T, C) # re-assemble all head outputs side by side
        # output projection
        y = self.c_proj(y)
        return y

class MLP(nn.Module):

    def __init__(self, config):
        super().__init__()
        self.c_fc    = nn.Linear(config.n_embd, 4 * config.n_embd)
        self.gelu    = NewGELU()
        self.c_proj  = nn.Linear(4 * config.n_embd, config.n_embd)

    def forward(self, x):
        x = self.c_fc(x)
        x = self.gelu(x)
        x = self.c_proj(x)
        return x

class Block(nn.Module):

    def __init__(self, config):
        super().__init__()
        self.ln_1 = nn.LayerNorm(config.n_embd)
        self.attn = CausalSelfAttention(config)
        self.ln_2 = nn.LayerNorm(config.n_embd)
        self.mlp = MLP(config)

    def forward(self, x):
        x = x + self.attn(self.ln_1(x))
        x = x + self.mlp(self.ln_2(x))
        return x

@dataclass
class GPTConfig:
    block_size: int = 1024
    vocab_size: int = 50257
    n_layer: int = 12
    n_head: int = 12
    n_embd: int = 768

class GPT(nn.Module):

    def __init__(self, config):
        super().__init__()
        self.config = config

        self.transformer = nn.ModuleDict(dict(
            wte = nn.Embedding(config.vocab_size, config.n_embd),
            wpe = nn.Embedding(config.block_size, config.n_embd),
            h = nn.ModuleList([Block(config) for _ in range(config.n_layer)]),
            ln_f = nn.LayerNorm(config.n_embd),
        ))
        self.lm_head = nn.Linear(config.n_embd, config.vocab_size, bias=False)
        self.transformer.wte.weight = self.lm_head.weight # https://paperswithcode.com/method/weight-tying

    def forward(self, idx, targets=None):
        device = idx.device
        b, t = idx.size()
        assert t <= self.config.block_size, f"Cannot forward sequence of length {t}, block size is only {self.config.block_size}"
        pos = torch.arange(0, t, dtype=torch.long, device=device) # shape (t)

        # forward the GPT model itself
        tok_emb = self.transformer.wte(idx) # token embeddings of shape (b, t, n_embd)
        pos_emb = self.transformer.wpe(pos) # position embeddings of shape (t, n_embd)
        x = tok_emb + pos_emb

        for block in self.transformer.h:
            x = block(x)
        x = self.transformer.ln_f(x)

        if targets is not None:
            # if we are given some desired targets also calculate the loss
            logits = self.lm_head(x)
            loss = F.cross_entropy(logits.view(-1, logits.size(-1)), targets.view(-1), ignore_index=-1)
        else:
            # inference-time mini-optimization: only forward the lm_head on the very last position
            logits = self.lm_head(x[:, [-1], :]) # note: using list [-1] to preserve the time dim
            loss = None

        return logits, loss

    @classmethod
    def from_pretrained(cls, model_type):
        """Loads pretrained GPT-2 model weights from huggingface"""
        assert model_type in {'gpt2', 'gpt2-medium', 'gpt2-large', 'gpt2-xl'}
        from transformers import GPT2LMHeadModel
        print("loading weights from pretrained gpt: %s" % model_type)

        # n_layer, n_head and n_embd are determined from model_type
        config_args = {
            'gpt2':         dict(n_layer=12, n_head=12, n_embd=768),  # 124M params
            'gpt2-medium':  dict(n_layer=24, n_head=16, n_embd=1024), # 350M params
            'gpt2-large':   dict(n_layer=36, n_head=20, n_embd=1280), # 774M params
            'gpt2-xl':      dict(n_layer=48, n_head=25, n_embd=1600), # 1558M params
        }[model_type]
        config_args['vocab_size'] = 50257 # always 50257 for GPT model checkpoints
        config_args['block_size'] = 1024 # always 1024 for GPT model checkpoints
        # create a from-scratch initialized minGPT model
        config = GPTConfig(**config_args)
        model = GPT(config)
        sd = model.state_dict()
        sd_keys = sd.keys()
        sd_keys = [k for k in sd_keys if not k.endswith('.attn.bias')] # discard this mask / buffer, not a param

        # init a huggingface/transformers model
        model_hf = GPT2LMHeadModel.from_pretrained(model_type)
        sd_hf = model_hf.state_dict()

        # copy while ensuring all of the parameters are aligned and match in names and shapes
        sd_keys_hf = sd_hf.keys()
        sd_keys_hf = [k for k in sd_keys_hf if not k.endswith('.attn.masked_bias')] # ignore these, just a buffer
        sd_keys_hf = [k for k in sd_keys_hf if not k.endswith('.attn.bias')] # same, just the mask (buffer)
        transposed = ['attn.c_attn.weight', 'attn.c_proj.weight', 'mlp.c_fc.weight', 'mlp.c_proj.weight']
        # basically the openai checkpoints use a "Conv1D" module, but we only want to use a vanilla Linear
        # this means that we have to transpose these weights when we import them
        assert len(sd_keys_hf) == len(sd_keys), f"mismatched keys: {len(sd_keys_hf)} != {len(sd_keys)}"
        for k in sd_keys_hf:
            if any(k.endswith(w) for w in transposed):
                # special treatment for the Conv1D weights we need to transpose
                assert sd_hf[k].shape[::-1] == sd[k].shape
                with torch.no_grad():
                    sd[k].copy_(sd_hf[k].t())
            else:
                # vanilla copy over the other parameters
                assert sd_hf[k].shape == sd[k].shape
                with torch.no_grad():
                    sd[k].copy_(sd_hf[k])

        return model

    @torch.no_grad()
    def generate(self, idx, max_new_tokens, temperature=1.0, top_k=None):
        """
        Take a conditioning sequence of indices idx (LongTensor of shape (b,t)) and complete
        the sequence max_new_tokens times, feeding the predictions back into the model each time.
        Most likely you'll want to make sure to be in model.eval() mode of operation for this.
        """
        for _ in range(max_new_tokens):
            # if the sequence context is growing too long we must crop it at block_size
            idx_cond = idx if idx.size(1) <= self.config.block_size else idx[:, -self.config.block_size:]
            # forward the model to get the logits for the index in the sequence
            logits, _ = self(idx_cond)
            # pluck the logits at the final step and scale by desired temperature
            logits = logits[:, -1, :] / temperature
            # optionally crop the logits to only the top k options
            if top_k is not None:
                v, _ = torch.topk(logits, min(top_k, logits.size(-1)))
                logits[logits < v[:, [-1]]] = -float('Inf')
            # apply softmax to convert logits to (normalized) probabilities
            probs = F.softmax(logits, dim=-1)
            # sample from the distribution
            idx_next = torch.multinomial(probs, num_samples=1)
            # append sampled index to the running sequence and continue
            idx = torch.cat((idx, idx_next), dim=1)

        return idx

# a few utilities for saving params/grads/activations to files for loading in C
def write_fp32(tensor, file):
    t = tensor.detach().cpu().to(torch.float32)
    b = t.numpy().tobytes()
    file.write(b)

def write_bf16(tensor, file):
    t = tensor.detach().cpu().to(torch.bfloat16)
    # numpy can't convert bf16 to bytes
    # this way below *i think* works, but is SUPER slow or broken
    # TODO fix :'(
    b = bytes(t.untyped_storage())
    file.write(b)

def write_tensors_fp32(model_tensors, L, file):
    write_fp32(model_tensors["transformer.wte.weight"], file) # (V, C)
    write_fp32(model_tensors["transformer.wpe.weight"], file) # (T, C)
    for i in range(L): # (L, 3C, C)
        write_fp32(model_tensors[f"transformer.h.{i}.attn.c_attn.weight"], file)
    for i in range(L): # (L, 3C)
        write_fp32(model_tensors[f"transformer.h.{i}.attn.c_attn.bias"], file)
    for i in range(L): # (L, C, C)
        write_fp32(model_tensors[f"transformer.h.{i}.attn.c_proj.weight"], file)
    for i in range(L): # (L, C)
        write_fp32(model_tensors[f"transformer.h.{i}.attn.c_proj.bias"], file)
    for i in range(L): # (L, 4C, C)
        write_fp32(model_tensors[f"transformer.h.{i}.mlp.c_fc.weight"], file)
    for i in range(L): # (L, 4C)
        write_fp32(model_tensors[f"transformer.h.{i}.mlp.c_fc.bias"], file)
    for i in range(L): # (L, C, 4C)
        write_fp32(model_tensors[f"transformer.h.{i}.mlp.c_proj.weight"], file)
    for i in range(L): # (L, C)
        write_fp32(model_tensors[f"transformer.h.{i}.mlp.c_proj.bias"], file)

    for i in range(L): # (L, C)
        write_fp32(model_tensors[f"transformer.h.{i}.ln_1.weight"], file)
    for i in range(L): # (L, C)
        write_fp32(model_tensors[f"transformer.h.{i}.ln_1.bias"], file)
    for i in range(L): # (L, C)
        write_fp32(model_tensors[f"transformer.h.{i}.ln_2.weight"], file)
    for i in range(L): # (L, C)
        write_fp32(model_tensors[f"transformer.h.{i}.ln_2.bias"], file)

    write_fp32(model_tensors["transformer.ln_f.weight"], file) # (C, )
    write_fp32(model_tensors["transformer.ln_f.bias"], file) # (C, )

def write_tensors_bf16(model_tensors, L, file):
    # same as fp32, but note we will re-order the tensors
    # because we keep the layernorm in fp32, we place them all at the end
    write_bf16(model_tensors["transformer.wte.weight"], file) # (V, C)
    write_bf16(model_tensors["transformer.wpe.weight"], file) # (T, C)
    for i in range(L): # (L, 3C, C)
        write_bf16(model_tensors[f"transformer.h.{i}.attn.c_attn.weight"], file)
    for i in range(L): # (L, 3C)
        write_bf16(model_tensors[f"transformer.h.{i}.attn.c_attn.bias"], file)
    for i in range(L): # (L, C, C)
        write_bf16(model_tensors[f"transformer.h.{i}.attn.c_proj.weight"], file)
    for i in range(L): # (L, C)
        write_bf16(model_tensors[f"transformer.h.{i}.attn.c_proj.bias"], file)
    for i in range(L): # (L, 4C, C)
        write_bf16(model_tensors[f"transformer.h.{i}.mlp.c_fc.weight"], file)
    for i in range(L): # (L, 4C)
        write_bf16(model_tensors[f"transformer.h.{i}.mlp.c_fc.bias"], file)
    for i in range(L): # (L, C, 4C)
        write_bf16(model_tensors[f"transformer.h.{i}.mlp.c_proj.weight"], file)
    for i in range(L): # (L, C)
        write_bf16(model_tensors[f"transformer.h.{i}.mlp.c_proj.bias"], file)
    # LayerNorms are at the end and kept in fp32
    for i in range(L): # (L, C)
        write_fp32(model_tensors[f"transformer.h.{i}.ln_1.weight"], file)
    for i in range(L): # (L, C)
        write_fp32(model_tensors[f"transformer.h.{i}.ln_1.bias"], file)
    for i in range(L): # (L, C)
        write_fp32(model_tensors[f"transformer.h.{i}.ln_2.weight"], file)
    for i in range(L): # (L, C)
        write_fp32(model_tensors[f"transformer.h.{i}.ln_2.bias"], file)
    write_fp32(model_tensors["transformer.ln_f.weight"], file) # (C, )
    write_fp32(model_tensors["transformer.ln_f.bias"], file) # (C, )

def write_model(model, filename, dtype):
    # everything we need to instantiate the model
    # 1) header is: version int, GPTConfig ints, padding to 1024 bytes
    assert dtype in {"float32", "bfloat16"} # float16 todo maybe later
    version = {
        "float32": 1,
        "bfloat16": 2,
    }[dtype]
    header = torch.zeros(256, dtype=torch.int32)
    header[0] = 20240326 # magic
    header[1] = version # checkpoint version
    header[2] = model.config.block_size
    header[3] = model.config.vocab_size
    header[4] = model.config.n_layer
    header[5] = model.config.n_head
    header[6] = model.config.n_embd
    # 2) the parameters follow the header
    params = {name: param.cpu() for name, param in model.named_parameters()}
    with open(filename, "wb") as file:
        # write header
        file.write(header.numpy().tobytes())
        # write params
        if dtype == "float32":
            write_tensors_fp32(params, model.config.n_layer, file)
        elif dtype == "bfloat16":
            write_tensors_bf16(params, model.config.n_layer, file)
    print(f"wrote {filename}")

def write_state(model, x, y, logits, loss, filename):
    # the state is used for debugging.
    # it contains information about the input, logits, loss, and the parameter gradients
    # this can be used for checking the computation correctness in C
    header = torch.zeros(256, dtype=torch.int32)
    header[0] = 20240327 # magic
    header[1] = 1 # run state version = 1
    header[2] = x.size(0) # batch size of the batch, B
    header[3] = x.size(1) # temporal extent of the batch, T
    grads = {name: param.grad.cpu() for name, param in model.named_parameters()}
    with open(filename, "wb") as file:
        # header
        file.write(header.numpy().tobytes())
        # input x
        file.write(x.cpu().numpy().astype("int32").tobytes()) # (B, T)
        # targets y
        file.write(y.cpu().numpy().astype("int32").tobytes()) # (B, T)
        # logits (result of the model forward pass)
        write_fp32(logits.cpu(), file)
        # loss (single float, result of the cross entropy loss)
        write_fp32(loss.cpu(), file)
        # gradients
        write_tensors_fp32(grads, model.config.n_layer, file)
    print(f"wrote {filename}")

def write_tokenizer(enc, filename):
    n = enc.max_token_value + 1
    header = torch.zeros(256, dtype=torch.int32)
    header[0] = 20240328 # magic
    header[1] = 1 # tokenizer version = 1
    header[2] = n # number of tokens
    with open(filename, "wb") as file:
        file.write(header.numpy().tobytes())
        for i in range(n):
            b = enc.decode_bytes([i])
            length = len(b)
            assert length < 256, f"Token length exceeds 255: {length}"
            file.write(struct.pack("<B", length))  # Write the length as a 1-byte unsigned integer
            file.write(b)  # Write the actual bytes
    print(f"wrote {filename}")

if __name__ == "__main__":
    import time
    import argparse
    import tiktoken
    print(f"Running pytorch {torch.version.__version__}")

    # default settings will overfit a tiny batch of data
    # and save model weights and debug state to disk on the first iteration
    # if you'd like to e.g. time the forward pass only, call this script as:
    # python train_gpt2.py --inference_only 1 --write_tensors 0 --sequence_length 1024
    parser = argparse.ArgumentParser()
    parser.add_argument("--write_tensors", type=int, default=1, help="write tensors to disk")
    parser.add_argument("--inference_only", type=int, default=0, help="only run inference")
    parser.add_argument("--dtype", type=str, default="float32", help="float32|float16|bfloat16")
    parser.add_argument("--device", type=str, default="", help="by default we autodetect, or set it here")
    parser.add_argument("--compile", type=int, default=0, help="torch.compile the model")
    parser.add_argument("--tensorcores", type=int, default=0, help="use tensorcores")
    parser.add_argument("--num_iterations", type=int, default=10, help="number of iterations to run")
    parser.add_argument("--batch_size", type=int, default=4, help="batch size")
    parser.add_argument("--sequence_length", type=int, default=64, help="sequence length")
    args = parser.parse_args()
    B, T = args.batch_size, args.sequence_length
    assert 1 <= T <= 1024
    assert args.dtype in {"float32", "float16", "bfloat16"}

    # select the device
    if args.device:
        device = args.device
    else:
        # attempt to autodetect the device
        device = "cpu"
        if torch.cuda.is_available():
            device = "cuda"
        elif hasattr(torch.backends, "mps") and torch.backends.mps.is_available():
            device = "mps"
    print(f"using device: {device}")

    # set up a context manager following the desired dtype and device
    ptdtype = {'float32': torch.float32, 'bfloat16': torch.bfloat16, 'float16': torch.float16}[args.dtype]
    ctx = torch.amp.autocast(device_type="cuda", dtype=ptdtype) if device == "cuda" else nullcontext()

    # seed the random number generators
    torch.manual_seed(42)
    if torch.cuda.is_available():
        torch.cuda.manual_seed(42)

    # set the torch precision mode to use TensorFloat32 (TF32) for matmuls
    # docs https://pytorch.org/docs/stable/generated/torch.set_float32_matmul_precision.html
    if args.tensorcores:
        torch.set_float32_matmul_precision('high')

    # init (and write) the tokenizer
    enc = tiktoken.get_encoding("gpt2")
    encode = lambda s: enc.encode(s, allowed_special={"<|endoftext|>"})
    decode = lambda l: enc.decode(l)
    write_tokenizer(enc, "gpt2_tokenizer.bin")

    # load the GPT-2 model weights
    model = GPT.from_pretrained("gpt2")
    model.train()
    model.to(device)
    if args.compile:
        if hasattr(config, "coordinate_descent_tuning"):
            config.coordinate_descent_tuning = True # suggested by @Chillee
        print("compiling the model...")
        model = torch.compile(model)

    # -------------------------------------------------------------------------
    # data loading related: long but it's just to get a single batch of data

    # load the tokens
    # prefer to use tiny_shakespeare if it's available, otherwise use tiny_stories
    # we're using val instead of train split just because it is smaller/faster
    shake_tokens_bin = "data/tiny_shakespeare_val.bin"
    story_tokens_bin = "data/TinyStories_val.bin"
    assert os.path.isfile(shake_tokens_bin) or os.path.isfile(story_tokens_bin), "you must run prepro on some dataset"
    tokens_bin = shake_tokens_bin if os.path.isfile(shake_tokens_bin) else story_tokens_bin
    assert os.path.isfile(tokens_bin)
    print(f"loading cached tokens in {tokens_bin}")
    with open(tokens_bin, "rb") as f:
        tokens = np.frombuffer(f.read(), dtype=np.int32)

    # np -> tensor, long, on device
    tokens = torch.tensor(tokens)
    tokens = tokens.to(torch.long)
    tokens = tokens.to(device)

    # lightweight dataloader
    def get_batch():
        assert B*T+1 <= len(tokens), "not enough tokens"
        # for 338,025 tokens. E.g. with B=8 T=1024, this will yield 41 batches before looping
        i = 0
        while True:
            x = tokens[i:i+B*T].view(B, T)
            y = tokens[i+1:i+B*T+1].view(B, T)
            yield x, y
            i += B*T
            if i + B*T + 1 >= len(tokens):
                i = 0 # in prod we'd want to randomize the start point a bit

    # fetch one batch of data, which we will overfit to
    data_iter = iter(get_batch())
    x, y = next(data_iter) # we'll overfit this batch below

    # -------------------------------------------------------------------------
    # STAGE 1: weights / state logging for C to load later

    # do one forward pass to generate ground truth for our C tests
    if not args.inference_only and args.write_tensors:
        logits, loss = model(x, y)
        loss.backward()
        # save model params, in both float32 and bfloat16
        write_model(model, "gpt2_124M.bin", dtype="float32")
        # write_model(model, "gpt2_124M_bf16.bin", dtype="bfloat16")
        # save x, y, logits, loss, and parameter gradients, for debugging C
        # always store these in fp32 to have an accurate reference (?)
        write_state(model, x, y, logits, loss, "gpt2_124M_debug_state.bin")

    # -------------------------------------------------------------------------
    # STAGE 2: training loop to get timings

    # init the optimizer
    adam_use_fused = device == "cuda" # only works on CUDA (?)
    optimizer = torch.optim.Adam(model.parameters(), lr=1e-4, fused=adam_use_fused)

    if device == "cuda":
        torch.cuda.reset_peak_memory_stats()
    timings = []
    for i in range(args.num_iterations):
        t0 = time.time()
        with ctx:
            logits, loss = model(x, y)
            del logits
            if not args.inference_only:
                optimizer.zero_grad()
                loss.backward()
                optimizer.step()
        # wait on the CPU for all device work to end so we get accurate per-iteration timings below
        if device == "mps":
            torch.mps.synchronize()
        elif device == "cuda":
            torch.cuda.synchronize()
        # time and print
        t1 = time.time()
        # the 0th iteration is often an outlier (much slower) => skip logging it
        if i > 0 and i > args.num_iterations - 20:
            timings.append(t1-t0)
        print(f"iteration {i}, loss: {loss.item()}, time: {(t1-t0)*1000:.3f}ms")

    # print the average of the last 20 timings, to get something smooth-ish
    timings = timings[-20:]
    print(f"final {len(timings)} iters avg: {np.mean(timings)*1000:.3f}ms")
    print(f"peak memory consumption: {torch.cuda.max_memory_allocated() // 1024 // 1024} MiB")

    # -------------------------------------------------------------------------
    # STAGE 3: Few steps of inference

    # before we end, let's also do one round of inference
    # we'll kick off the generation with "<|endoftext|>", which designates the start of a new sequence
    start = "<|endoftext|>"
    start_ids = encode(start)
    x = (torch.tensor(start_ids, dtype=torch.long, device=device)[None, ...])

    # run generation for 16 time steps (tokens)
    max_new_tokens = 16
    temperature = 1.0
    top_k = 40
    model.eval()
    y = model.generate(x, max_new_tokens, temperature=temperature, top_k=top_k)
    print(decode(y[0].tolist()))
    print('---------------')
