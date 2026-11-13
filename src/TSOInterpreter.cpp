/* Copyright (C) 2014-2017 Carl Leonardsson
 *
 * This file is part of Nidhugg.
 *
 * Nidhugg is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Nidhugg is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include "Debug.h"
#include "TSOInterpreter.h"

#include <llvm/IR/LLVMContext.h>

#include <memory>
#include <string>
#include <utility>

static void SetValue(llvm::Value *V, llvm::GenericValue Val, llvm::ExecutionContext &SF) {
  SF.Values[V] = Val;
}

TSOInterpreter::TSOInterpreter(llvm::Module *M, TSOTraceBuilder &TB,
                               const Configuration &conf)
  : Interpreter(M,TB,conf) {
  tso_threads.push_back(TSOThread());
}

TSOInterpreter::~TSOInterpreter(){
}

std::unique_ptr<TSOInterpreter> TSOInterpreter::
create(llvm::Module *M, TSOTraceBuilder &TB, const Configuration &conf,
       std::string *ErrorStr){
  if (llvm::Error Err = M->materializeAll()) {
    std::string Msg;
    handleAllErrors(std::move(Err), [&](llvm::ErrorInfoBase &EIB) {
      Msg = EIB.message();
    });
    if (ErrorStr)
      *ErrorStr = Msg;
    // We got an error, just return 0
    return nullptr;
  }

  return std::unique_ptr<TSOInterpreter>(new TSOInterpreter(M,TB,conf));
}

void TSOInterpreter::runAux(int proc, int aux){
  /* Perform an update from store buffer to memory. */

  assert(aux == 0);
  assert(tso_threads[proc].store_buffer.size());

  void *ref = tso_threads[proc].store_buffer.front().first;
  const SymData &blk = tso_threads[proc].store_buffer.front().second;

  if(!TB.atomic_store(blk)) { abort(); return; }

  if(DryRun) return;

  std::memcpy((uint8_t*)ref,(uint8_t*)blk.get_block(),blk.get_ref().size);
  CheckAwaitWakeup(llvm::GenericValue(), ref, blk.get_ref());

  for(unsigned i = 0; i < tso_threads[proc].store_buffer.size()-1; ++i){
    tso_threads[proc].store_buffer[i] = tso_threads[proc].store_buffer[i+1];
  }
  tso_threads[proc].store_buffer.pop_back();

  if(int(tso_threads[proc].store_buffer.size()) <= tso_threads[proc].partial_buffer_flush){
    assert(0 <= tso_threads[proc].partial_buffer_flush);
    /* The real thread was waiting for the buffer to flush. Enable the
     * real thread. */
    tso_threads[proc].partial_buffer_flush = -1;
    TB.mark_available(proc);
  }

  if(tso_threads[proc].store_buffer.empty()){
    /* Disable update thread. */
    TB.mark_unavailable(proc,0);
    /* If the real thread has terminated, then wake up other threads
     * which are waiting to join with this one. */
    if(Threads[proc].ECStack.empty()){
      for(int p : Threads[proc].AwaitingJoin){
        TB.mark_available(p);
      }
    }
  }
}

int TSOInterpreter::newThread(const CPid &cpid){
  tso_threads.push_back(TSOThread());
  return Interpreter::newThread(cpid);
}

bool TSOInterpreter::isFence(llvm::Instruction &I){
  if(llvm::CallInst *CI = llvm::dyn_cast<llvm::CallInst>(&I)){
    llvm::Function *F = CI->getCalledFunction();
    if(F && F->getName().str().rfind("__VERIFIER_load_await", 0) == 0) return false; //needed? maybe put verifier to extfun_no_fence? xchg?
    if(F && F->isDeclaration() &&
       F->getIntrinsicID() == llvm::Intrinsic::not_intrinsic &&
       conf.extfun_no_fence.count(F->getName().str()) == 0){
      return true;
    }
    if(F && F->getName().str().find("__VERIFIER_atomic_") == 0) return true;
    {
      std::string asmstr;
      if(isInlineAsm(AnyCallInst(CI),&asmstr) && asmstr == "mfence") return true;
    }
  }else if(llvm::isa<llvm::StoreInst>(I)){
    return static_cast<llvm::StoreInst&>(I).getOrdering() == llvm::AtomicOrdering::SequentiallyConsistent;
  }else if(llvm::isa<llvm::FenceInst>(I)){
    return static_cast<llvm::FenceInst&>(I).getOrdering() == llvm::AtomicOrdering::SequentiallyConsistent;
  }else if(llvm::isa<llvm::AtomicCmpXchgInst>(I)){
    llvm::AtomicOrdering succ = static_cast<llvm::AtomicCmpXchgInst&>(I).getSuccessOrdering();
    llvm::AtomicOrdering fail = static_cast<llvm::AtomicCmpXchgInst&>(I).getFailureOrdering();
    if(succ != llvm::AtomicOrdering::SequentiallyConsistent || fail != llvm::AtomicOrdering::SequentiallyConsistent){
      Debug::warn("TSOInterpreter::isFence::cmpxchg") << "WARNING: Non-sequentially consistent CMPXCHG instruction interpreted as sequentially consistent.\n";
    }
    return true;
  }else if(llvm::isa<llvm::AtomicRMWInst>(I)){
    if(static_cast<llvm::AtomicRMWInst&>(I).getOrdering() != llvm::AtomicOrdering::SequentiallyConsistent){
      Debug::warn("TSOInterpreter::isFence::rmw") << "WARNING: Non-sequentially consistent RMW instruction interpreted as sequentially consistent.\n";
    }
    return true;
  }
  return false;
}

void TSOInterpreter::terminate(llvm::Type *RetTy, llvm::GenericValue Result){
  if(CurrentThread != 0){
#if LLVM_VERSION_MAJOR >= 18
    assert(RetTy == llvm::PointerType::get(RetTy->getContext(), 0));
#else
    assert(RetTy == llvm::Type::getInt8PtrTy(RetTy->getContext()));
#endif
    Threads[CurrentThread].RetVal = Result;
  }
  if(tso_threads[CurrentThread].store_buffer.empty()){
    for(int p : Threads[CurrentThread].AwaitingJoin){
      TB.mark_available(p);
    }
  }
}

bool TSOInterpreter::checkRefuse(llvm::Instruction &I){
  int tid;
  if(isPthreadJoin(I,&tid)){
    if(0 <= tid && tid < int(Threads.size()) && tid != CurrentThread){
      if(Threads[tid].ECStack.size() ||
         tso_threads[tid].store_buffer.size()){
        /* The awaited thread is still executing */
        TB.refuse_schedule();
        Threads[tid].AwaitingJoin.push_back(CurrentThread);
        return true;
      }
    }else{
      // Erroneous thread id
      // Allow execution (will produce an error trace)
    }
  }
  /* Refuse if I has fence semantics and the store buffer is
   * non-empty.
   */
  if(isFence(I) && !tso_threads[CurrentThread].store_buffer.empty()){
    tso_threads[CurrentThread].partial_buffer_flush = 0;
    TB.refuse_schedule();
    return true;
  }
  /* Refuse if I is a load and the latest entry in the store buffer
   * which overlaps with the memory location targeted by I does not
   * target precisely the same bytes as I.
   */
  if(llvm::isa<llvm::LoadInst>(I)){
    llvm::ExecutionContext &SF = ECStack()->back();
    llvm::GenericValue SRC = getOperandValue(static_cast<llvm::LoadInst&>(I).getPointerOperand(), SF);
    llvm::GenericValue *Ptr = (llvm::GenericValue*)GVTOP(SRC);
    Option<SymAddrSize> mr = TryGetSymAddrSize(Ptr,static_cast<llvm::LoadInst&>(I).getType());
    if (!mr) return false; /* Let it execute and segfault */
    for(int i = int(tso_threads[CurrentThread].store_buffer.size())-1; 0 <= i; --i){
      if(mr->overlaps(tso_threads[CurrentThread].store_buffer[i].second.get_ref())){
        if(*mr != tso_threads[CurrentThread].store_buffer[i].second.get_ref()){
          /* Block until this store buffer entry has disappeared from
           * the buffer.
           */
          tso_threads[CurrentThread].partial_buffer_flush =
            int(tso_threads[CurrentThread].store_buffer.size()) - i - 1;
          TB.refuse_schedule();
          return true;
        }
        break;
      }
    }
  }
  /* Refuse if I is a load await and the latest entry in the store buffer
   * which overlaps with the memory location targeted by I does not
   * target precisely the same bytes as I, or does not satisfy the condition.
   */
  {
    llvm::GenericValue *ptr;
    AwaitCond cond;
    if(isLoadAwait(I, &ptr, &cond)){
      Option<SymAddrSize> mr = TryGetSymAddrSize(ptr, I.getOperand(2)->getType());
      if (!mr) {
        Debug::warn("await-bad-addr")
          << "Await will segfault";
        /* Bad address, let execute and crash */
        return false;
      }
      const void *data_ptr = (const void*)ptr;
      for(int i = int(tso_threads[CurrentThread].store_buffer.size())-1; 0 <= i; --i){
        if(mr->overlaps(tso_threads[CurrentThread].store_buffer[i].second.get_ref())){
          if(*mr != tso_threads[CurrentThread].store_buffer[i].second.get_ref()){
            /* Block until this store buffer entry has disappeared from
             * the buffer.
             */
            tso_threads[CurrentThread].partial_buffer_flush =
              int(tso_threads[CurrentThread].store_buffer.size()) - i - 1;
            TB.refuse_schedule();
            return true;
          }
          assert(!(DryRun && DryRunMem.size()));
          if (!cond.satisfied_by(tso_threads[CurrentThread].store_buffer[i].second)) {
            if (!TB.load_await_fail(*mr, cond)) {
              abort();
              return true;
            }
            TB.refuse_schedule();
            blocking_awaits[*mr].emplace(CurrentThread, std::move(cond));
            return true;
          }
          return false;
        }
      }
    }
  }
  return Interpreter::checkRefuse(I);
}

void TSOInterpreter::visitLoadInst(llvm::LoadInst &I){
  llvm::ExecutionContext &SF = ECStack()->back();
  llvm::GenericValue SRC = getOperandValue(I.getPointerOperand(), SF);
  llvm::GenericValue *Ptr = (llvm::GenericValue*)GVTOP(SRC);
  llvm::GenericValue Result;

  Option<SymAddrSize> Ptr_sas = GetSymAddrSize(Ptr,I.getType());
  if (!Ptr_sas) return;
  if(!TB.load(*Ptr_sas)) { abort(); return; }

  if(DryRun && DryRunMem.size()){
    assert(tso_threads[CurrentThread].store_buffer.empty());
    DryRunLoadValueFromMemory(Result, Ptr, *Ptr_sas, I.getType());
    SetValue(&I, Result, SF);
    return;
  }

  /* Check store buffer for ROWE opportunity. */
  for(int i = int(tso_threads[CurrentThread].store_buffer.size())-1; 0 <= i; --i){
    if(Ptr_sas->addr == tso_threads[CurrentThread].store_buffer[i].second.get_ref().addr){
      /* Read-Own-Write-Early */
      assert(Ptr_sas->size == tso_threads[CurrentThread].store_buffer[i].second.get_ref().size);
      LoadValueFromMemory(Result,(llvm::GenericValue*)tso_threads[CurrentThread].store_buffer[i].second.get_block(),I.getType());
      SetValue(&I, Result, SF);
      return;
    }
  }

  /* Load from memory */
  LoadValueFromMemory(Result, Ptr, I.getType());
  SetValue(&I, Result, SF);
}

void TSOInterpreter::visitStoreInst(llvm::StoreInst &I){
  llvm::ExecutionContext &SF = ECStack()->back();
  llvm::GenericValue Val = getOperandValue(I.getOperand(0), SF);
  llvm::GenericValue *Ptr = (llvm::GenericValue *)GVTOP(getOperandValue(I.getPointerOperand(), SF));
  Option<SymData> sd = GetSymData(Ptr, I.getOperand(0)->getType(), Val);
  if (!sd) return;

  if(I.getOrdering() == llvm::AtomicOrdering::SequentiallyConsistent ||
     0 <= AtomicFunctionCall){
    /* Atomic store */
    assert(tso_threads[CurrentThread].store_buffer.empty());
    if(!TB.atomic_store(*sd)) { abort(); return; }
    if(DryRun){
      DryRunMem.push_back(std::move(*sd));
      return;
    }
    StoreValueToMemory(Val, Ptr, I.getOperand(0)->getType());
    CheckAwaitWakeup(llvm::GenericValue(), Ptr, sd->get_ref());
  }else{
    /* Store to buffer */
    if(!TB.store(*sd)) { abort(); return; }
    if(DryRun){
      DryRunMem.push_back(std::move(*sd));
      return;
    }
    tso_threads[CurrentThread].store_buffer.emplace_back(Ptr, std::move(*sd));
  }
}

void TSOInterpreter::visitFenceInst(llvm::FenceInst &I){
  if(I.getOrdering() == llvm::AtomicOrdering::SequentiallyConsistent){
    if(!TB.fence()) { abort(); return; }
  }
}

void TSOInterpreter::visitAtomicCmpXchgInst(llvm::AtomicCmpXchgInst &I){
  assert(tso_threads[CurrentThread].store_buffer.empty());
  Interpreter::visitAtomicCmpXchgInst(I);
}

void TSOInterpreter::visitAtomicRMWInst(llvm::AtomicRMWInst &I){
  assert(tso_threads[CurrentThread].store_buffer.empty());
  Interpreter::visitAtomicRMWInst(I);
}

void TSOInterpreter::visitInlineAsm(llvm::CallInst &CS, const std::string &asmstr){
  if(asmstr == "mfence"){
    if(!TB.fence()) { abort(); return; }
  }else if(asmstr == ""){ // Do nothing
  }else{
    throw std::logic_error("Unsupported inline assembly: "+asmstr);
  }
}

void TSOInterpreter::callLoadAwait(llvm::Function *F,
                                   const std::vector<llvm::GenericValue> &ArgVals) {
  llvm::GenericValue *Ptr = (llvm::GenericValue*)GVTOP(ArgVals[0]);
  uint64_t op_int = ArgVals[1].IntVal.getZExtValue();
  AwaitCond::Op op(static_cast<AwaitCond::Op>(op_int));
   /* Should be arg2 type, but if well-formed is same as return type */
  llvm::Type *Ty = F->getReturnType();
  SymData::block_type operand = SymData::alloc_block(getDataLayout().getTypeStoreSize(Ty));
  StoreValueToMemory(ArgVals[2],
                     static_cast<llvm::GenericValue*>((void*)operand.get()), Ty);

  llvm::GenericValue Result;
  Option<SymAddrSize> Ptr_sas = GetSymAddrSize(Ptr, Ty);
  if (!Ptr_sas) return;

  if (!TB.load_await(*Ptr_sas, AwaitCond{op, std::move(operand)})) {
    abort();
    return;
  }

  /* Check store buffer for ROWE opportunity */
  for (int i = int(tso_threads[CurrentThread].store_buffer.size()) - 1; 0 <= i; --i) {
    if (Ptr_sas->addr == tso_threads[CurrentThread].store_buffer[i].second.get_ref().addr) {
      /* Read-Own-Write-Early */
      assert(Ptr_sas->size == tso_threads[CurrentThread].store_buffer[i].second.get_ref().size);
      LoadValueFromMemory(Result, (llvm::GenericValue*)tso_threads[CurrentThread].store_buffer[i].second.get_block(), Ty);
      returnValueToCaller(F->getReturnType(), Result);
      return;
    }
  }

  /* Load from memory */
  assert(!(DryRun && DryRunMem.size()));
  LoadValueFromMemory(Result, Ptr, Ty);
  returnValueToCaller(F->getReturnType(), Result);
}

void TSOInterpreter::callXchgAwait(llvm::Function *F, const std::vector<llvm::GenericValue> &ArgVals) {
  assert(tso_threads[CurrentThread].store_buffer.empty());
  Interpreter::callXchgAwait(F, ArgVals);
}

void TSOInterpreter::CheckAwaitWakeup(const llvm::GenericValue &Val, const void *ptr,
                                      const SymAddrSize &sas) {
  assert(!DryRun);

  for (auto sas_awaits_iter = blocking_awaits.begin();
       sas_awaits_iter != blocking_awaits.end();) {
    const SymAddrSize &await_sas = sas_awaits_iter->first;
    if (!await_sas.overlaps(sas)) {
      ++sas_awaits_iter;
      continue;
    }
    const void *await_ptr =
      static_cast<const uint8_t *>(ptr) + (await_sas.addr - sas.addr);
    auto &sas_awaits = sas_awaits_iter->second;

    for (auto it = sas_awaits.begin(); it != sas_awaits.end();) {
      int tid = it->first;
      bool shadowed = false;

      /* Check if this thread has pending stores to the await sas in its store buffer */
      for (int i = int(tso_threads[tid].store_buffer.size()) - 1; 0 <= i; --i) {
        if (await_sas.overlaps(tso_threads[tid].store_buffer[i].second.get_ref())) {
          /* Ignore checking this blocked await since it can't read from memory */
          shadowed = true;
          break;
        }
      }

      if (!shadowed && it->second.satisfied_by(await_ptr, await_sas.size)) {
        TB.mark_available(tid);
        it = sas_awaits.erase(it);
      } else {
        ++it;
      }
    }
    if (sas_awaits.empty()) {
      sas_awaits_iter = blocking_awaits.erase(sas_awaits_iter);
    } else {
      ++sas_awaits_iter;
    }
  }
}
