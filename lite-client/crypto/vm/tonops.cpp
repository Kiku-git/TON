#include <functional>
#include "vm/tonops.h"
#include "vm/log.h"
#include "vm/opctable.h"
#include "vm/stack.hpp"
#include "vm/continuation.h"
#include "vm/excno.hpp"
#include "Ed25519.h"

namespace vm {

namespace {

bool debug(const char* str) {
  std::cerr << str;
  return true;
}

bool debug(int x) {
  if (x < 100) {
    std::cerr << '[' << (char)(64 + x) << ']';
  } else {
    std::cerr << '[' << (char)(64 + x / 100) << x % 100 << ']';
  }
  return true;
}
}  // namespace

#define DBG_START int dbg = 0;
#define DBG debug(++dbg)&&
#define DEB_START DBG_START
#define DEB DBG

int exec_set_gas_generic(VmState* st, long long new_gas_limit) {
  if (new_gas_limit < st->gas_consumed()) {
    throw VmNoGas{};
  }
  st->change_gas_limit(new_gas_limit);
  return 0;
}

int exec_accept(VmState* st) {
  VM_LOG(st) << "execute ACCEPT";
  return exec_set_gas_generic(st, GasLimits::infty);
}

int exec_set_gas_limit(VmState* st) {
  VM_LOG(st) << "execute SETGASLIMIT";
  td::RefInt256 x = st->get_stack().pop_int_finite();
  long long gas = 0;
  if (x->sgn() > 0) {
    gas = x->unsigned_fits_bits(63) ? x->to_long() : GasLimits::infty;
  }
  return exec_set_gas_generic(st, gas);
}

void register_basic_gas_ops(OpcodeTable& cp0) {
  using namespace std::placeholders;
  cp0.insert(OpcodeInstr::mksimple(0xf800, 16, "ACCEPT", exec_accept))
      .insert(OpcodeInstr::mksimple(0xf801, 16, "SETGASLIMIT", exec_set_gas_limit));
}

void register_ton_gas_ops(OpcodeTable& cp0) {
  using namespace std::placeholders;
}

int exec_get_param(VmState* st, unsigned idx, const char* name) {
  VM_LOG(st) << "execute " << name;
  Stack& stack = st->get_stack();
  auto tuple = st->get_c7();
  auto t1 = tuple_index(*tuple, 0).as_tuple_range(255);
  if (t1.is_null()) {
    throw VmError{Excno::type_chk, "intermediate value is not a tuple"};
  }
  stack.push(tuple_index(*t1, idx));
  return 0;
}

void register_ton_config_ops(OpcodeTable& cp0) {
  using namespace std::placeholders;
  cp0.insert(OpcodeInstr::mksimple(0xf823, 16, "NOW", std::bind(exec_get_param, _1, 3, "NOW")))
      .insert(OpcodeInstr::mksimple(0xf824, 16, "BLOCKLT", std::bind(exec_get_param, _1, 4, "BLOCKLT")))
      .insert(OpcodeInstr::mksimple(0xf825, 16, "LTIME", std::bind(exec_get_param, _1, 5, "LTIME")));
}

int exec_compute_hash(VmState* st, int mode) {
  VM_LOG(st) << "execute HASH" << (mode & 1 ? 'S' : 'C') << 'U';
  Stack& stack = st->get_stack();
  std::array<unsigned char, 32> hash;
  if (!(mode & 1)) {
    auto cell = stack.pop_cell();
    hash = cell->get_hash().as_array();
  } else {
    auto cs = stack.pop_cellslice();
    vm::CellBuilder cb;
    CHECK(cb.append_cellslice_bool(std::move(cs)));
    // TODO: use cb.get_hash() instead
    hash = cb.finalize()->get_hash().as_array();
  }
  td::RefInt256 res{true};
  CHECK(res.write().import_bytes(hash.data(), hash.size(), false));
  stack.push_int(std::move(res));
  return 0;
}

int exec_ed25519_check_signature(VmState* st) {
  VM_LOG(st) << "execute CHKSIGNU";
  Stack& stack = st->get_stack();
  stack.check_underflow(3);
  auto key_int = stack.pop_int();
  auto signature_cs = stack.pop_cellslice();
  auto hash_int = stack.pop_int();
  unsigned char hash[32], key[32], signature[64];
  if (!hash_int->export_bytes(hash, 32, false)) {
    throw VmError{Excno::range_chk, "data hash must fit in an unsigned 256-bit integer"};
  }
  if (!signature_cs->prefetch_bytes(signature, 64)) {
    throw VmError{Excno::cell_und, "Ed25519 signature must contain at least 512 data bits"};
  }
  if (!key_int->export_bytes(key, 32, false)) {
    throw VmError{Excno::range_chk, "Ed25519 public key must fit in an unsigned 256-bit integer"};
  }
  td::Ed25519::PublicKey pub_key{td::Slice{key, 32}};
  auto res = pub_key.verify_signature(td::Slice{hash, 32}, td::Slice{signature, 64});
  stack.push_bool(res.is_ok());
  return 0;
}

void register_ton_crypto_ops(OpcodeTable& cp0) {
  using namespace std::placeholders;
  cp0.insert(OpcodeInstr::mksimple(0xf900, 16, "HASHCU", std::bind(exec_compute_hash, _1, 0)))
      .insert(OpcodeInstr::mksimple(0xf901, 16, "HASHSU", std::bind(exec_compute_hash, _1, 1)))
      .insert(OpcodeInstr::mksimple(0xf910, 16, "CHKSIGNU", exec_ed25519_check_signature));
}

int exec_load_var_integer(VmState* st, int len_bits, bool sgnd, bool quiet) {
  if (len_bits == 4 && sgnd) {
    VM_LOG(st) << "execute LDGRAMS" << (quiet ? "Q" : "");
  } else {
    VM_LOG(st) << "execute LDVAR" << (sgnd ? "" : "U") << "INT" << (1 << len_bits) << (quiet ? "Q" : "");
  }
  Stack& stack = st->get_stack();
  auto csr = stack.pop_cellslice();
  td::RefInt256 x;
  int len;
  if (!(csr.write().fetch_uint_to(len_bits, len) && csr.unique_write().fetch_int256_to(len * 8, x, sgnd))) {
    if (quiet) {
      stack.push_bool(false);
    } else {
      throw VmError{Excno::cell_und, "cannot deserialize a variable-length integer"};
    }
  } else {
    stack.push_int(std::move(x));
    stack.push_cellslice(std::move(csr));
    if (quiet) {
      stack.push_bool(true);
    }
  }
  return 0;
}

int exec_store_var_integer(VmState* st, int len_bits, bool sgnd, bool quiet) {
  if (len_bits == 4 && sgnd) {
    VM_LOG(st) << "execute STGRAMS" << (quiet ? "Q" : "");
  } else {
    VM_LOG(st) << "execute STVAR" << (sgnd ? "" : "U") << "INT" << (1 << len_bits) << (quiet ? "Q" : "");
  }
  Stack& stack = st->get_stack();
  stack.check_underflow(2);
  auto x = stack.pop_int();
  auto cbr = stack.pop_builder();
  unsigned len = ((x->bit_size(sgnd) + 7) >> 3);
  if (len >= (1u << len_bits)) {
    throw VmError{Excno::range_chk};
  }
  if (!(cbr.write().store_long_bool(len, len_bits) && cbr.unique_write().store_int256_bool(*x, len * 8, sgnd))) {
    if (quiet) {
      stack.push_bool(false);
    } else {
      throw VmError{Excno::cell_ov, "cannot serialize a variable-length integer"};
    }
  } else {
    stack.push_builder(std::move(cbr));
    if (quiet) {
      stack.push_bool(true);
    }
  }
  return 0;
}

bool skip_maybe_anycast(CellSlice& cs) {
  if (cs.prefetch_ulong(1) != 1) {
    return cs.advance(1);
  }
  unsigned depth;
  return cs.advance(1)                    // just$1
         && cs.fetch_uint_leq(30, depth)  // anycast_info$_ depth:(#<= 30)
         && depth >= 1                    // { depth >= 1 }
         && cs.advance(depth);            // rewrite_pfx:(bits depth) = Anycast;
}

bool skip_message_addr(CellSlice& cs) {
  switch ((unsigned)cs.fetch_ulong(2)) {
    case 0:  // addr_none$00 = MsgAddressExt;
      return true;
    case 1: {  // addr_extern$01
      unsigned len;
      return cs.fetch_uint_to(9, len)  // len:(## 9)
             && cs.advance(len);       // external_address:(bits len) = MsgAddressExt;
    }
    case 2: {                         // addr_std$10
      return skip_maybe_anycast(cs)   // anycast:(Maybe Anycast)
             && cs.advance(8 + 256);  // workchain_id:int8 address:bits256  = MsgAddressInt;
    }
    case 3: {  // addr_var$11
      unsigned len;
      return skip_maybe_anycast(cs)       // anycast:(Maybe Anycast)
             && cs.fetch_uint_to(9, len)  // addr_len:(## 9)
             && cs.advance(32 + len);     // workchain_id:int32 address:(bits addr_len) = MsgAddressInt;
    }
    default:
      return false;
  }
}

int exec_load_message_addr(VmState* st, bool quiet) {
  VM_LOG(st) << "execute LDMSGADDR" << (quiet ? "Q" : "");
  Stack& stack = st->get_stack();
  auto csr = stack.pop_cellslice(), csr_copy = csr;
  auto& cs = csr.write();
  if (!(skip_message_addr(cs) && csr_copy.write().cut_tail(cs))) {
    csr.clear();
    if (quiet) {
      stack.push_cellslice(std::move(csr_copy));
      stack.push_bool(false);
    } else {
      throw VmError{Excno::cell_und, "cannot load a MsgAddress"};
    }
  } else {
    stack.push_cellslice(std::move(csr_copy));
    stack.push_cellslice(std::move(csr));
    if (quiet) {
      stack.push_bool(true);
    }
  }
  return 0;
}

bool parse_maybe_anycast(CellSlice& cs, StackEntry& res) {
  res = StackEntry{};
  if (cs.prefetch_ulong(1) != 1) {
    return cs.advance(1);
  }
  unsigned depth;
  Ref<CellSlice> pfx;
  if (cs.advance(1)                           // just$1
      && cs.fetch_uint_leq(30, depth)         // anycast_info$_ depth:(#<= 30)
      && depth >= 1                           // { depth >= 1 }
      && cs.fetch_subslice_to(depth, pfx)) {  // rewrite_pfx:(bits depth) = Anycast;
    res = std::move(pfx);
    return true;
  }
  return false;
}

bool parse_message_addr(CellSlice& cs, std::vector<StackEntry>& res) {
  res.clear();
  switch ((unsigned)cs.fetch_ulong(2)) {
    case 0:                                      // addr_none$00 = MsgAddressExt;
      res.emplace_back(td::RefInt256{true, 0});  // -> (0)
      return true;
    case 1: {  // addr_extern$01
      unsigned len;
      Ref<CellSlice> addr;
      if (cs.fetch_uint_to(9, len)               // len:(## 9)
          && cs.fetch_subslice_to(len, addr)) {  // external_address:(bits len) = MsgAddressExt;
        res.emplace_back(td::RefInt256{true, 1});
        res.emplace_back(std::move(addr));
        return true;
      }
      break;
    }
    case 2: {  // addr_std$10
      StackEntry v;
      int workchain;
      Ref<CellSlice> addr;
      if (parse_maybe_anycast(cs, v)             // anycast:(Maybe Anycast)
          && cs.fetch_int_to(8, workchain)       // workchain_id:int8
          && cs.fetch_subslice_to(256, addr)) {  // address:bits256  = MsgAddressInt;
        res.emplace_back(td::RefInt256{true, 2});
        res.emplace_back(std::move(v));
        res.emplace_back(td::RefInt256{true, workchain});
        res.emplace_back(std::move(addr));
        return true;
      }
      break;
    }
    case 3: {  // addr_var$11
      StackEntry v;
      int len, workchain;
      Ref<CellSlice> addr;
      if (parse_maybe_anycast(cs, v)             // anycast:(Maybe Anycast)
          && cs.fetch_uint_to(9, len)            // addr_len:(## 9)
          && cs.fetch_int_to(32, workchain)      // workchain_id:int32
          && cs.fetch_subslice_to(len, addr)) {  // address:(bits addr_len) = MsgAddressInt;
        res.emplace_back(td::RefInt256{true, 3});
        res.emplace_back(std::move(v));
        res.emplace_back(td::RefInt256{true, workchain});
        res.emplace_back(std::move(addr));
        return true;
      }
      break;
    }
  }
  return false;
}

int exec_parse_message_addr(VmState* st, bool quiet) {
  VM_LOG(st) << "execute PARSEMSGADDR" << (quiet ? "Q" : "");
  Stack& stack = st->get_stack();
  auto csr = stack.pop_cellslice();
  auto& cs = csr.write();
  std::vector<StackEntry> res;
  if (!(parse_message_addr(cs, res) && cs.empty_ext())) {
    if (quiet) {
      stack.push_bool(false);
    } else {
      throw VmError{Excno::cell_und, "cannot parse a MsgAddress"};
    }
  } else {
    stack.push_tuple(std::move(res));
    if (quiet) {
      stack.push_bool(true);
    }
  }
  return 0;
}

void register_ton_currency_address_ops(OpcodeTable& cp0) {
  using namespace std::placeholders;
  cp0.insert(OpcodeInstr::mksimple(0xfa00, 16, "LDGRAMS", std::bind(exec_load_var_integer, _1, 4, true, false)))
      .insert(OpcodeInstr::mksimple(0xfa01, 16, "LDVARINT16", std::bind(exec_load_var_integer, _1, 4, false, false)))
      .insert(OpcodeInstr::mksimple(0xfa02, 16, "STGRAMS", std::bind(exec_store_var_integer, _1, 4, true, false)))
      .insert(OpcodeInstr::mksimple(0xfa03, 16, "STVARINT16", std::bind(exec_store_var_integer, _1, 4, false, false)))
      .insert(OpcodeInstr::mksimple(0xfa40, 16, "LDMSGADDR", std::bind(exec_load_message_addr, _1, false)))
      .insert(OpcodeInstr::mksimple(0xfa41, 16, "LDMSGADDRQ", std::bind(exec_load_message_addr, _1, true)))
      .insert(OpcodeInstr::mksimple(0xfa42, 16, "PARSEMSGADDR", std::bind(exec_parse_message_addr, _1, false)))
      .insert(OpcodeInstr::mksimple(0xfa43, 16, "PARSEMSGADDRQ", std::bind(exec_parse_message_addr, _1, true)));
}

static constexpr int output_actions_idx = 5;

int install_output_action(VmState* st, Ref<Cell> new_action_head) {
  // TODO: increase actions:uint16 and msgs_sent:uint16 in SmartContractInfo at first reference of c5
  VM_LOG(st) << "installing an output action";
  st->set_d(output_actions_idx, std::move(new_action_head));
  return 0;
}

static inline Ref<Cell> get_actions(VmState* st) {
  return st->get_d(output_actions_idx);
}

int exec_send_raw_message(VmState* st) {
  VM_LOG(st) << "execute SENDRAWMSG";
  Stack& stack = st->get_stack();
  stack.check_underflow(2);
  int f = stack.pop_smallint_range(255);
  Ref<Cell> msg_cell = stack.pop_cell();
  CellBuilder cb;
  if (!(cb.store_ref_bool(get_actions(st))     // out_list$_ {n:#} prev:^(OutList n)
        && cb.store_long_bool(0x0ec3c86d, 32)  // action_send_msg#0ec3c86d
        && cb.store_long_bool(f, 8)            // mode:(## 8)
        && cb.store_ref_bool(std::move(msg_cell)))) {
    throw VmError{Excno::cell_ov, "cannot serialize raw output message into an output action cell"};
  }
  return install_output_action(st, cb.finalize());
}

bool store_grams(CellBuilder& cb, td::RefInt256 value) {
  int k = value->bit_size(false);
  return k <= 15 * 8 && cb.store_long_bool((k + 7) >> 3, 4) && cb.store_int256_bool(*value, (k + 7) & -8, false);
}

int exec_reserve_raw(VmState* st, int mode) {
  VM_LOG(st) << "execute RESERVERAW" << (mode & 1 ? "X" : "");
  Stack& stack = st->get_stack();
  stack.check_underflow(2);
  int f = stack.pop_smallint_range(3);
  td::RefInt256 x;
  Ref<CellSlice> csr;
  if (mode & 1) {
    csr = stack.pop_cellslice();
  } else {
    x = stack.pop_int_finite();
    if (td::sgn(x) < 0) {
      throw VmError{Excno::range_chk, "amount of nanograms must be non-negative"};
    }
  }
  CellBuilder cb;
  if (!(cb.store_ref_bool(get_actions(st))     // out_list$_ {n:#} prev:^(OutList n)
        && cb.store_long_bool(0x36e6b809, 32)  // action_reserve_currency#36e6b809
        && cb.store_long_bool(f, 8)            // mode:(## 8)
        && (mode & 1 ? cb.append_cellslice_bool(std::move(csr))
                     : (store_grams(cb, std::move(x)) && cb.store_bool_bool(false))))) {
    throw VmError{Excno::cell_ov, "cannot serialize raw reserved currency amount into an output action cell"};
  }
  return install_output_action(st, cb.finalize());
}

void register_ton_message_ops(OpcodeTable& cp0) {
  using namespace std::placeholders;
  cp0.insert(OpcodeInstr::mksimple(0xfb00, 16, "SENDRAWMSG", exec_send_raw_message))
      .insert(OpcodeInstr::mksimple(0xfb02, 16, "RESERVERAW", std::bind(exec_reserve_raw, _1, 0)))
      .insert(OpcodeInstr::mksimple(0xfb03, 16, "RESERVERAWX", std::bind(exec_reserve_raw, _1, 1)));
}

void register_ton_ops(OpcodeTable& cp0) {
  register_basic_gas_ops(cp0);
  register_ton_gas_ops(cp0);
  register_ton_config_ops(cp0);
  register_ton_crypto_ops(cp0);
  register_ton_currency_address_ops(cp0);
  register_ton_message_ops(cp0);
}

}  // namespace vm
