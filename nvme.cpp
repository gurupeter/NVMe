// This file is a part of the IncludeOS unikernel - www.includeos.org
//
// Copyright 2015 Oslo and Akershus University College of Applied Sciences
// and Alfred Bratterud
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "nvme.hpp"

#include <kernel/events.hpp>
#include <statman>
#include <fs/common.hpp>
#include <hw/pci.hpp>
#include <cassert>
#include "nvme_regs.hpp"

#define NVME_RESET_VALUE  0x4E564D65 /* "NVMe" */
static const int SUBM_Q_SIZE = 16;
static const int COMP_Q_SIZE = 16;

static NVMe::queue_reference comp_ref(const nvme_io_comp_entry& entry) {
  return (entry.sq_id << 16) | entry.cmd_id;
}

NVMe::NVMe(hw::PCI_Device& dev)
  : m_pcidev(dev), m_aq(*this)
{
  INFO("NVMe", "|  [NVM express, initializing]");
  dev.probe_resources();
  dev.parse_capabilities();
  {
    auto& reqs = Statman::get().create(
      Stat::UINT32, device_name() + ".requests");
    this->m_requests = &reqs.get_uint32();
    *this->m_requests = 0;

    auto& err = Statman::get().create(
      Stat::UINT32, device_name() + ".errors");
    this->m_errors = &err.get_uint32();
    *this->m_errors = 0;
  }

  if (dev.msix_cap())
  {
    dev.init_msix();
    INFO2("|  |- Found %u MSI-x vectors", dev.get_msix_vectors());
    assert(dev.get_msix_vectors() >= 2);
    uint8_t acq = Events::get().subscribe({this, &NVMe::msix_aq_comp_handler});
    dev.setup_msix_vector(SMP::cpu_id(), IRQ_BASE + acq);
    uint8_t iocq = Events::get().subscribe({this, &NVMe::msix_ioq_comp_handler});
    dev.setup_msix_vector(SMP::cpu_id(), IRQ_BASE + iocq);
  }
  else {
    assert(0 && "No intx support for NVMe");
  }

  // controller registers BAR
  this->m_ctl = dev.get_bar(0).start;
  // verify NVM express version
  check_version();

  this->m_dbstride = (read64(REG_CAP) >> 32) & 0xF; // 32-35

  // turn off device
  write32(REG_CTLCFG, read32(REG_CTLCFG) & ~CFG_EN);
  //write32(REG_NVMSSR, NVME_RESET_VALUE);

  // create queues
  write32(REG_AQ_CFG,
      COMP_Q_SIZE << 16 |
      SUBM_Q_SIZE << 0  );

  // admin queue doesn't need to be bigger 2/2
  new (&m_aq) queue(*this, 0, 2, 2);
  write64(REG_AQ_SUBM_BA, (uint64_t) m_aq.subm.m_data);
  write64(REG_AQ_COMP_BA, (uint64_t) m_aq.comp.m_data);

  // configure & start device
  write32(REG_CTLCFG,
      4 << 20 | // compq entry exponent (16 bytes)
      6 << 16 | // submq entry exponent (64 bytes)
      0 << 11 | // round robin arbitration
      0 << 7  | // host page size (2^12)
      0 << 4  | // NVM command set
      CFG_EN);  // start device

  uint32_t status;
  do {
    asm("pause");
    status = read32(REG_CTLSTA);
  } while ((status & 0x3) == 0);
  if (status & 0x2) {
    printf("Failed to start NVMe device, ready = %d\n", status & 1);
    assert(0 && "NVMe fatal status");
  }
  assert(status & 0x1);

  write32(reg_doorbell_compq_head(ADMIN_Q, m_dbstride), 0);

  this->retrieve_information();
  assert(!this->m_aq.ns.empty() && "Must have at least one namespace");
  INFO("NVMe", "Block device with %zu sectors capacity", this->size());

  this->setup_io_queues();
}

void NVMe::check_version()
{
  const uint32_t reg = read32(REG_VER);
  const uint16_t major = reg >> 16;
  const uint16_t minor = (reg >> 8) & 0xFF;
  INFO2("|  |- NVM Express v%u.%u", major, minor);
  assert(major == 1);
  assert(minor == 0 || minor == 1 || minor == 2 || minor == 3);
}

void NVMe::retrieve_information()
{
  void* buffer = std::aligned_alloc(4096, 4096);

  // CNS 0x1 => Identify ctrl data
  auto res = this->m_aq.identify(0, 0x1, buffer);
  assert(res.good());
  auto* ctrl = (identify_ctrl_data*) buffer;
  INFO2("Identifiers: %#x, %#x", ctrl->vid, ctrl->ssvid);
  INFO2("Serial: %.*s", 20, ctrl->serial_number);
  INFO2("Model:  %.*s", 40, ctrl->model_number);
  INFO2("Version: %.*s", 8, ctrl->firmware_rev);

  std::free(buffer);
  this->m_aq.identify_namespaces();
}

void NVMe::setup_io_queues()
{
  const int qcount = 1;
  const uint32_t qdata = (qcount - 1) | ((qcount - 1) << 16);
  auto res = this->m_aq.set_features(NVME_FEAT_NUM_QUEUES, qdata, nullptr);
  assert(res.good());
  // NOTE: res.result contains two DWs containing the final count
  //printf("Done setting features\n");

  m_ioqs.emplace_back(*this, 1, SUBM_Q_SIZE, COMP_Q_SIZE);
  //printf("Done creating I/O queue\n");
}

NVMe::block_t NVMe::block_size() const noexcept {
  return m_aq.ns[0].block_size();
}
NVMe::block_t NVMe::size() const noexcept {
  return m_aq.ns[0].blocks();
}

void NVMe::msix_aq_comp_handler()
{
  //printf("NVMe::msix_aq_comp_handler()\n");
  this->handle_queue( this->m_aq );
}
void NVMe::msix_ioq_comp_handler()
{
  //printf("NVMe::msix_ioq_comp_handler()\n");
  this->handle_queue( this->m_ioqs.at(0) );
}
void NVMe::handle_queue(queue& q)
{
  while (true)
  {
    auto& entry = q.comp.comp_entry();
    if (entry.phase_tag() != q.comp.current_phase) break;
    printf("cmd id %#x status %#x   phase %#x\n",
           entry.cmd_id, entry.error(), entry.phase_tag());
    printf("--> SQ ID %#x SQ HEAD %#x\n", entry.sq_id, entry.sq_head);
    if (entry.error()) {
      printf("CQ %d error: %#x\n", entry.sq_id, entry.error_code());
    }
    assert(entry.error() == 0);
    auto copy = entry;
    // goto next item
    q.comp_advance_head();
    // process item
    q.handle_result(copy);
  }
  while (!q.full() && !q.writeq.empty())
  {
    auto item = std::move(q.writeq.front());
    q.writeq.pop_front();
    this->schedule(std::move(item));
  }
}
void NVMe::queue::handle_result(const nvme_io_comp_entry& entry)
{
  printf("Entry: status %#x\n", entry.error());
  printf("Him: %#x  Us1: %#x\n", comp_ref(entry), m_dev.async_results[0].uid);
  printf("Him: %#x  Us2: %#x\n", comp_ref(entry), m_dev.async_results[1].uid);
  printf("Him: %#x  Us3: %#x\n", comp_ref(entry), m_dev.async_results[2].uid);
  printf("Him: %#x  Us4: %#x\n", comp_ref(entry), m_dev.async_results[3].uid);

  const queue_reference uid = comp_ref(entry);
  assert(m_dev.async_results.front().uid == uid);
  auto result = std::move(m_dev.async_results.front());
  m_dev.async_results.pop_front();

  /* do something */
  switch (result.mode) {
  case MODE_READ:
        result.on_read(std::move(result.buffer));
        break;
  case MODE_WRITE:
        result.on_write(true);
        break;
  default:
        throw std::runtime_error("Unknown mode");
  }
}

void NVMe::read(block_t blk, size_t cnt, on_read_func callback)
{
  const work_item item {
    .blk  = blk,
    .cnt  = cnt,
    .mode = MODE_READ,
    .async = true,
    .buffer = fs::construct_buffer(block_size() * cnt),
    .on_read = std::move(callback)
  };
  this->schedule(std::move(item));
}
NVMe::buffer_t NVMe::read_sync(block_t blk, size_t cnt)
{
  const work_item item {
    .blk  = blk,
    .cnt  = cnt,
    .mode = MODE_READ,
    .async = false,
    .buffer = fs::construct_buffer(block_size() * cnt)
  };
  return this->begin_read(std::move(item));
}

NVMe::buffer_t NVMe::begin_read(work_item item)
{
  assert(item.mode == MODE_READ);
  const uint32_t nsid = m_aq.ns.at(0).nsid();
  auto& q = m_ioqs.at(0);
  auto cmd = q.read(nsid, item.buffer->data(), item.blk, item.cnt);
  if (item.async)
  {
    async_result result {
      .mode = MODE_READ,
      .buffer = std::move(item.buffer),
      .on_read = std::move(item.on_read)
    };
    q.submit_async(cmd, std::move(result));
    return nullptr;
  }
  else
  {
    auto res = q.submit_sync(cmd);
    if (res.good()) return item.buffer;
    return nullptr;
  }
}

void NVMe::write(block_t blk, buffer_t buffer, on_write_func callback)
{
  const work_item item {
    .blk  = blk,
    .cnt  = buffer->size() / block_size(),
    .mode = MODE_WRITE,
    .async = true,
    .buffer = std::move(buffer),
    .on_write = std::move(callback)
  };
  this->schedule(std::move(item));
}
bool NVMe::write_sync(block_t blk, buffer_t buffer)
{
  const work_item item {
    .blk  = blk,
    .cnt  = buffer->size() / block_size(),
    .mode = MODE_WRITE,
    .async = false,
    .buffer = std::move(buffer)
  };
  return this->begin_write(std::move(item));
}

bool NVMe::begin_write(work_item item)
{
  // TODO: wait for enough room to submit
  assert(item.mode == MODE_WRITE);
  auto& q = m_ioqs.at(0);
  auto cmd = q.write(m_aq.ns.at(0).nsid(),
                     item.buffer->data(), item.blk, item.cnt);
  if (item.async)
  {
    async_result result {
      .mode = MODE_WRITE,
      .buffer = std::move(item.buffer),
      .on_write = std::move(item.on_write)
    };
    q.submit_async(cmd, std::move(result));
    return true;
  }
  else
  {
    auto res = q.submit_sync(cmd);
    return res.good();
  }
}

void NVMe::schedule(work_item item)
{
  assert(item.async == true);
  auto& q = m_ioqs.at(0);
  if (q.full()) {
    q.writeq.push_back(std::move(item));
    printf("Queued up work item, size %zu\n", q.writeq.size());
    return;
  }
  switch (item.mode) {
    case MODE_READ:
        this->begin_read(std::move(item));
        break;
    case MODE_WRITE:
        this->begin_write(std::move(item));
        break;
  }
}

void NVMe::deactivate()
{
  /// TODO: reset device
}

void NVMe::queue::identify_namespaces()
{
  void* buffer = std::aligned_alloc(4096, 4096);
  // CNS 0x2 => Identify namespaces
  auto res = this->identify(0, 0x2, buffer);
  assert(res.good());
  // iterate namespaces until zero
  auto* idlist = (uint32_t*) buffer;
  for (int i = 0; i < 1024; i++)
  {
    if (idlist[i] == 0) break;
    this->attach_namespace(idlist[i]);
  }
  std::free(buffer);
}
void NVMe::queue::attach_namespace(const uint32_t nsid)
{
  INFO("NVMe", "Attaching namespace %#x", nsid);
  this->ns.emplace_back(m_dev, *this, nsid);
}

NVMe::namespace_t::namespace_t(NVMe& dev, queue& q, uint32_t nsid)
  : m_dev(dev), m_nsid(nsid)
{
  void* buffer = std::aligned_alloc(4096, 4096);

  // CNS 0x0 => Identify namespace data
  auto res = q.identify(this->m_nsid, 0x0, buffer);
  assert(res.good());

  auto* d = (identify_namespace_data*) buffer;

  const int lbaf_idx = d->FLBAS & 0xF;
  this->m_blk_size = 1 << d->LBAF[lbaf_idx].LBADS;
  INFO2("Block size: %lu", this->m_blk_size);
  this->m_blocks = d->NSZE;
  INFO2("Blocks: %lu", this->m_blocks);

  std::free(buffer);
}

NVMe::queue::queue(NVMe& dev, const int qid,
    const uint16_t SUBM_SIZE, const uint16_t COMP_SIZE)
  : m_dev(dev)
{
  subm.alloc(qid, SUBM_SIZE, sizeof(nvme_command));
  comp.alloc(qid, COMP_SIZE, sizeof(nvme_io_comp_entry));
  if (qid > 0)
  {
    nvme_command cmd;
    cmd.opcode = NVME_CMD_CREATE_CQ;
    cmd.create_cq.prp1  = (uint64_t) comp.m_data;
    cmd.create_cq.cq_id = qid;
    cmd.create_cq.cq_size = COMP_SIZE-1; // ??
    cmd.create_cq.cq_flags = NVME_QUEUE_PHYS_CONTIG | NVME_CQ_IRQ_ENABLED;
    cmd.create_cq.cq_vector = qid;
    auto res = m_dev.m_aq.submit_sync(cmd);
    assert(res.good());
    cmd = {};
    cmd.opcode = NVME_CMD_CREATE_SQ;
    cmd.create_sq.prp1  = (uint64_t) subm.m_data;
    cmd.create_sq.sq_id = qid;
    cmd.create_sq.sq_size = SUBM_SIZE-1; // ??
    cmd.create_sq.sq_flags = NVME_QUEUE_PHYS_CONTIG;
    cmd.create_sq.sq_cqid = qid;
    res = m_dev.m_aq.submit_sync(cmd);
    assert(res.good());
  }
}
void NVMe::queue_ring::alloc(const uint16_t idx, const uint16_t size, const size_t elem)
{
  this->m_data = std::aligned_alloc(4096, elem * size);
  memset(this->m_data, 0, elem * size);
  this->no   = idx;
  this->size = size;
}

NVMe::sync_result NVMe::queue::identify(
      const uint32_t nsid, const uint32_t cns, void* dma_addr)
{
  nvme_command cmd;
  cmd.opcode  = NVME_CMD_IDENTIFY;
  cmd.nsid    = nsid;
  cmd.ident.prp1 = (uint64_t) dma_addr;
  cmd.ident.cns  = cns;
  return this->submit_sync(cmd);
}

NVMe::sync_result NVMe::queue::set_features(
      const uint32_t fid, const uint32_t dw11, void* dma_addr)
{
  nvme_command cmd;
  cmd.opcode = NVME_CMD_SET_FEATURES;
  cmd.features.prp1 = (uint64_t) dma_addr;
  cmd.features.fid  = fid;
  cmd.features.dw11 = dw11;
  return this->submit_sync(cmd);
}

NVMe::sync_result NVMe::queue::create_ioq(const uint32_t nsid)
{
  nvme_command cmd;
  cmd.opcode = NVME_CMD_CREATE_CQ;
  cmd.nsid   = nsid;
  cmd.entry.prp1 = 0;
  cmd.entry.dw10 = 0;
  printf("Create I/O queue with Q1=%p Q2=%p\n", nullptr, nullptr);
  return this->submit_sync(cmd);
}

nvme_command NVMe::queue::read(uint32_t nsid, void* buffer, uint64_t lba, uint16_t blks)
{
  nvme_command cmd;
  cmd.opcode = NVME_IO_READ;
  cmd.nsid   = nsid;
  cmd.rw.prp1   = (uint64_t) buffer;
  cmd.rw.slba   = lba;
  cmd.rw.length = blks-1;
  return cmd;
}
nvme_command NVMe::queue::write(uint32_t nsid, void* buffer, uint64_t lba, uint16_t blks)
{
  nvme_command cmd;
  cmd.opcode = NVME_IO_WRITE;
  cmd.nsid   = nsid;
  cmd.rw.prp1   = (uint64_t) buffer;
  cmd.rw.slba   = lba;
  cmd.rw.length = blks-1;
  return cmd;
}

void NVMe::queue::submit_async(nvme_command& cmd, async_result async)
{
  const uint32_t uid = this->self_reference();
  async.uid = uid;
  m_dev.async_results.push_back(std::move(async));
  // generate command id
  const uint16_t cid = id_counter++;
  cmd.cmd_id = cid;
  //printf("async uid: %#x  cid %#x\n", uid, cid);
  // submit command
  this->submit(cmd);
}
NVMe::sync_result NVMe::queue::submit_sync(nvme_command& cmd)
{
  // generate command id
  const uint16_t cid = id_counter++;
  cmd.cmd_id = cid;
  // submit command
  this->submit(cmd);
  // wait for command to complete
  while (true)
  {
    auto& entry = comp.comp_entry();
    if (entry.phase_tag() == comp.current_phase) break;
    _mm_pause();
  }
  auto& entry = comp.comp_entry();
  // check for errors
  assert(entry.cmd_id == cid);
  if (entry.error())
  {
    printf("Error: %#x\n", entry.error_code());
    // increment head in the ring
    this->comp_advance_head();
    // return error
    return sync_result{-1, 0};
  }
  const sync_result result{0, entry.result};
  // increment head in the ring
  this->comp_advance_head();
  // return result
  return result;
}
void NVMe::queue::submit(nvme_command& cmd)
{
  auto& q = this->subm;
  // write entry to queue area
  q.command(q.index, m_dev.m_dbstride) = cmd;

  // update tail pointer
  q.index = (q.index + 1) % q.size;
  m_dev.write32(reg_doorbell_submq_tail(q.no, m_dev.m_dbstride), q.index);
  assert(this->level < q.size);
  this->level ++;
}

NVMe::queue_reference NVMe::queue::self_reference() const noexcept
{
  return (this->subm.no << 16) | this->id_counter;
}

nvme_command& NVMe::queue_ring::command(const uint16_t idx, const uint32_t dbstride)
{
  assert(idx < this->size);
  return ((nvme_command*) this->m_data) [idx];
}
nvme_io_comp_entry& NVMe::queue_ring::comp_entry() noexcept
{
  return ((nvme_io_comp_entry*) this->m_data)[this->index];
}
void NVMe::queue_ring::advance_head(NVMe& dev) noexcept
{
  this->index++;
  if (this->index == this->size) {
    this->index = 0;
    this->current_phase = 1 - this->current_phase;
  }
  dev.write32(reg_doorbell_compq_head(this->no, dev.m_dbstride), this->index);
}
void NVMe::queue::comp_advance_head() noexcept
{
  this->comp.advance_head(m_dev);
  assert(this->level > 0);
  this->level--;
}

uint32_t NVMe::read32(const uint32_t off) noexcept
{
  assert((off & 3) == 0);
  return *(volatile uint32_t*) (this->m_ctl + off);
}
void NVMe::write32(const uint32_t off, const uint32_t val) noexcept
{
  assert((off & 3) == 0);
  *(volatile uint32_t*) (this->m_ctl + off) = val;
}
uint64_t NVMe::read64(const uint32_t off) noexcept
{
  assert((off & 7) == 0);
  return *(volatile uint64_t*) (this->m_ctl + off);
}
void NVMe::write64(const uint32_t off, const uint64_t val) noexcept
{
  assert((off & 7) == 0);
  *(volatile uint64_t*) (this->m_ctl + off) = val;
}

#include <hw/pci_manager.hpp>
__attribute__((constructor))
static void nvme_gconstr() {
  // QEMU NVM Express Controller
  hw::PCI_manager::register_blk(PCI::VENDOR_INTEL, 0x5845, &NVMe::new_instance);
}
