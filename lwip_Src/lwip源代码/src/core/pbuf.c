/**
 * @file
 * Packet buffer management
 *
 * Packets are built from the pbuf data structure. It supports dynamic
 * memory allocation for packet contents or can reference externally
 * managed packet contents both in RAM and ROM. Quick allocation for
 * incoming packets is provided through pools with fixed sized pbufs.
 *
 * A packet may span over multiple pbufs, chained as a singly linked
 * list. This is called a "pbuf chain".
 *
 * Multiple packets may be queued, also using this singly linked list.
 * This is called a "packet queue".
 * 
 * So, a packet queue consists of one or more pbuf chains, each of
 * which consist of one or more pbufs. CURRENTLY, PACKET QUEUES ARE
 * NOT SUPPORTED!!! Use helper structs to queue multiple packets.
 * 
 * The differences between a pbuf chain and a packet queue are very
 * precise but subtle. 
 *
 * The last pbuf of a packet has a ->tot_len field that equals the
 * ->len field. It can be found by traversing the list. If the last
 * pbuf of a packet has a ->next field other than NULL, more packets
 * are on the queue.
 *
 * Therefore, looping through a pbuf of a single packet, has an
 * loop end condition (tot_len == p->len), NOT (next == NULL).
 */

/*
 * Copyright (c) 2001-2004 Swedish Institute of Computer Science.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 *
 * Author: Adam Dunkels <adam@sics.se>
 *
 */


#include "lwip/opt.h" m /* debug.h在该文件中已包含 */

#include "lwip/stats.h"
#include "lwip/def.h"
#include "lwip/mem.h"
#include "lwip/memp.h"
#include "lwip/pbuf.h"
#include "lwip/sys.h"
#include "arch/perf.h"
#if LWIP_TCP && TCP_QUEUE_OOSEQ
#include "lwip/tcp_impl.h"
#endif
#if LWIP_CHECKSUM_ON_COPY
#include "lwip/inet_chksum.h"
#endif

#include <string.h>

/* 以下两个都是用于在PBUF_POOL内存池中或里面的单个完整pbuf中偏移时使用。因为memp_std.h中定义PBUF_POOL
 * 内存池时，里面的每一个完整pbuf的头和数据区都是分别对齐的，所以偏移时要使用对齐后的偏移量
 */
#define SIZEOF_STRUCT_PBUF        LWIP_MEM_ALIGN_SIZE(sizeof(struct pbuf))
/* Since the pool is created in memp, PBUF_POOL_BUFSIZE will be automatically
   aligned there. Therefore, PBUF_POOL_BUFSIZE_ALIGNED can be used here. */
#define PBUF_POOL_BUFSIZE_ALIGNED LWIP_MEM_ALIGN_SIZE(PBUF_POOL_BUFSIZE)


/* PBUF_POOL_IS_EMPTY()：检查PBUF_POOL内存池是否为空。
 * 仅当PUBF_POOL_FREE_OOSEQ功能开启时，会使用。
 */

/* 下面#if语句C语言讲解：
 * PBUF_POOL_FREE_OOSEQ只有在前两者都为真时，才存在。但或运算符的特点是，当根据前面的表达式能
 * 判断处整个逻辑表达式的真假时，后面的表达式就不会计算了。
 * 所以当LWIP_TCP或TCP_QUEUE_OOSEQ为假时，虽然PBUF_POOL_FREE_OOSEQ并不存在，但仍不影响下面#if
 * 的判断。
 */
#if !LWIP_TCP || !TCP_QUEUE_OOSEQ || !PBUF_POOL_FREE_OOSEQ /* 如果pbuf free ooseq 功能没开 */
#define PBUF_POOL_IS_EMPTY()                               /* 则此函数就不存在 */
#else /* !LWIP_TCP || !TCP_QUEUE_OOSEQ || !PBUF_POOL_FREE_OOSEQ */
                                                           /* 否则，表示pbuf free ooseq功能开启 */
#if !NO_SYS       	                                       /* 则判断是否是有操作系统环境 */	
#ifndef PBUF_POOL_FREE_OOSEQ_QUEUE_CALL
#include "lwip/tcpip.h"                                    /*  */
#define PBUF_POOL_FREE_OOSEQ_QUEUE_CALL()  do { \    /* 暂时先不考虑 */
  if(tcpip_callback_with_block(pbuf_free_ooseq_callback, NULL, 0) != ERR_OK) { \
      SYS_ARCH_PROTECT(old_level); \
      pbuf_free_ooseq_pending = 0; \
      SYS_ARCH_UNPROTECT(old_level); \
  } } while(0)
#endif /* PBUF_POOL_FREE_OOSEQ_QUEUE_CALL */
#endif /* !NO_SYS */

volatile u8_t pbuf_free_ooseq_pending;
#define PBUF_POOL_IS_EMPTY() pbuf_pool_is_empty()

/**
 * Attempt to reclaim some memory from queued out-of-sequence TCP segments
 * if we run out of pool pbufs. It's better to give priority to new packets
 * if we're running out.
 *
 * This must be done in the correct thread context therefore this function
 * can only be used with NO_SYS=0 and through tcpip_callback.
 */
#if !NO_SYS			/* 只能在带操作系统时通过tcpip_callback使用，函数变成局部函数，以确保
                 * in the correct thread context */
static
#endif /* !NO_SYS */
void
pbuf_free_ooseq(void) /* 从当前TCP开始，在TCP队列中找到一个ooseq队列中存在ooseq的，然后将所有的ooseq都释放 */
{
  struct tcp_pcb* pcb;
  SYS_ARCH_DECL_PROTECT(old_level);

  SYS_ARCH_PROTECT(old_level);
  pbuf_free_ooseq_pending = 0;
  SYS_ARCH_UNPROTECT(old_level);

  for (pcb = tcp_active_pcbs; NULL != pcb; pcb = pcb->next) {
    if (NULL != pcb->ooseq) {
      /** Free the ooseq pbufs of one PCB only */
      LWIP_DEBUGF(PBUF_DEBUG | LWIP_DBG_TRACE, ("pbuf_free_ooseq: freeing out-of-sequence pbufs\n"));
      tcp_segs_free(pcb->ooseq);  /* 全部释放ooseq */
      pcb->ooseq = NULL;
      return;
    }
  }
}

#if !NO_SYS              /* 带操作系统时才存在 */
/**
 * Just a callback function for tcpip_timeout() that calls pbuf_free_ooseq().
 */
static void
pbuf_free_ooseq_callback(void *arg)
{
  LWIP_UNUSED_ARG(arg);
  pbuf_free_ooseq();
}
#endif /* !NO_SYS */
/* ********************************以上free ooseq,带操作系统************************************************ */


/** Queue a call to pbuf_free_ooseq if not already queued. */
static void
pbuf_pool_is_empty(void)
{
#ifndef PBUF_POOL_FREE_OOSEQ_QUEUE_CALL
  SYS_ARCH_DECL_PROTECT(old_level);
  SYS_ARCH_PROTECT(old_level);
  pbuf_free_ooseq_pending = 1;
  SYS_ARCH_UNPROTECT(old_level);
#else /* PBUF_POOL_FREE_OOSEQ_QUEUE_CALL */
  u8_t queued;
  SYS_ARCH_DECL_PROTECT(old_level);
  SYS_ARCH_PROTECT(old_level);
  queued = pbuf_free_ooseq_pending;
  pbuf_free_ooseq_pending = 1;
  SYS_ARCH_UNPROTECT(old_level);

  if(!queued) {
    /* queue a call to pbuf_free_ooseq if not already queued */
    PBUF_POOL_FREE_OOSEQ_QUEUE_CALL();
  }
#endif /* PBUF_POOL_FREE_OOSEQ_QUEUE_CALL */
}
#endif /* !LWIP_TCP || !TCP_QUEUE_OOSEQ || !PBUF_POOL_ FREE_OOSEQ */

/* *************************************以上free ooseq打开************************************ */




/**
 * Allocates a pbuf of the given type (possibly a chain for PBUF_POOL type).
 *
 * The actual memory allocated for the pbuf is determined by the
 * layer at which the pbuf is allocated and the requested size
 * (from the size parameter).
 *
 * @param layer flag to define header size
 * @param length size of the pbuf's payload
 * @param type this parameter decides how and where the pbuf
 * should be allocated as follows:
 *
 * - PBUF_RAM: buffer memory for pbuf is allocated as one large
 *             chunk. This includes protocol headers as well.
 * - PBUF_ROM: no buffer memory is allocated for the pbuf, even for
 *             protocol headers. Additional headers must be prepended
 *             by allocating another pbuf and chain in to the front of
 *             the ROM pbuf. It is assumed that the memory used is really
 *             similar to ROM in that it is immutable and will not be
 *             changed. Memory which is dynamic should generally not
 *             be attached to PBUF_ROM pbufs. Use PBUF_REF instead.
 * - PBUF_REF: no buffer memory is allocated for the pbuf, even for
 *             protocol headers. It is assumed that the pbuf is only
 *             being used in a single thread. If the pbuf gets queued,
 *             then pbuf_take should be called to copy the buffer.
 * - PBUF_POOL: the pbuf is allocated as a pbuf chain, with pbufs from
 *              the pbuf pool that is allocated during pbuf_init().
 *
 * @return the allocated pbuf. If multiple pbufs where allocated, this
 * is the first pbuf of a pbuf chain.
 */
struct pbuf *
pbuf_alloc(pbuf_layer layer, u16_t length, pbuf_type type)
{
  struct pbuf *p, *q, *r;
  u16_t offset;
  s32_t rem_len; /* remaining length */
  LWIP_DEBUGF(PBUF_DEBUG | LWIP_DBG_TRACE, ("pbuf_alloc(length=%"U16_F")\n", length));

  /* determine header offset */
  switch (layer) {
  case PBUF_TRANSPORT:
    /* add room for transport (often TCP) layer header */
    offset = PBUF_LINK_HLEN + PBUF_IP_HLEN + PBUF_TRANSPORT_HLEN;
    break;
  case PBUF_IP:
    /* add room for IP layer header */
    offset = PBUF_LINK_HLEN + PBUF_IP_HLEN;
    break;
  case PBUF_LINK:
    /* add room for link layer header */
    offset = PBUF_LINK_HLEN;
    break;
  case PBUF_RAW:
    offset = 0;
    break;
  default:
    LWIP_ASSERT("pbuf_alloc: bad pbuf layer", 0);
    return NULL;
  }

  switch (type) {
  case PBUF_POOL:
    /* allocate head of pbuf chain into p */
    p = (struct pbuf *)memp_malloc(MEMP_PBUF_POOL);
    LWIP_DEBUGF(PBUF_DEBUG | LWIP_DBG_TRACE, ("pbuf_alloc: allocated pbuf %p\n", (void *)p));
    if (p == NULL) {
      PBUF_POOL_IS_EMPTY(); /* 如果free ooseq功能开启，则释放ooseq */
      return NULL;
    }
    p->type = type;
    p->next = NULL;

    /* make the payload pointer point 'offset' bytes into pbuf data memory */
    p->payload = LWIP_MEM_ALIGN((void *)((u8_t *)p + (SIZEOF_STRUCT_PBUF + offset)));
    LWIP_ASSERT("pbuf_alloc: pbuf p->payload properly aligned",
            ((mem_ptr_t)p->payload % MEM_ALIGNMENT) == 0);
    /* the total length of the pbuf chain is the requested size */
    p->tot_len = length;
    /* set the length of the first pbuf in the chain */
    p->len = LWIP_MIN(length, PBUF_POOL_BUFSIZE_ALIGNED - LWIP_MEM_ALIGN_SIZE(offset));
    LWIP_ASSERT("check p->payload + p->len does not overflow pbuf",
                ((u8_t*)p->payload + p->len <=
                 (u8_t*)p + SIZEOF_STRUCT_PBUF + PBUF_POOL_BUFSIZE_ALIGNED));
    LWIP_ASSERT("PBUF_POOL_BUFSIZE must be bigger than MEM_ALIGNMENT",
      (PBUF_POOL_BUFSIZE_ALIGNED - LWIP_MEM_ALIGN_SIZE(offset)) > 0 );
    /* set reference count (needed here in case we fail) */
    p->ref = 1;

    /* now allocate the tail of the pbuf chain */

    /* remember first pbuf for linkage in next iteration */
    r = p;
    /* remaining length to be allocated */
    rem_len = length - p->len;
    /* any remaining pbufs to be allocated? */
    while (rem_len > 0) {
      q = (struct pbuf *)memp_malloc(MEMP_PBUF_POOL);
      if (q == NULL) {
        PBUF_POOL_IS_EMPTY();
        /* free chain so far allocated */
        pbuf_free(p);
        /* bail out unsuccesfully */
        return NULL;
      }
      q->type = type;
      q->flags = 0;
      q->next = NULL;
      /* make previous pbuf point to this pbuf */
      r->next = q;
      /* set total length of this pbuf and next in chain */
      LWIP_ASSERT("rem_len < max_u16_t", rem_len < 0xffff);
      q->tot_len = (u16_t)rem_len;
      /* this pbuf length is pool size, unless smaller sized tail */
      q->len = LWIP_MIN((u16_t)rem_len, PBUF_POOL_BUFSIZE_ALIGNED);
      q->payload = (void *)((u8_t *)q + SIZEOF_STRUCT_PBUF);
      LWIP_ASSERT("pbuf_alloc: pbuf q->payload properly aligned",
              ((mem_ptr_t)q->payload % MEM_ALIGNMENT) == 0);
      LWIP_ASSERT("check p->payload + p->len does not overflow pbuf",
                  ((u8_t*)p->payload + p->len <=
                   (u8_t*)p + SIZEOF_STRUCT_PBUF + PBUF_POOL_BUFSIZE_ALIGNED));
      q->ref = 1;
      /* calculate remaining length to be allocated */
      rem_len -= q->len;
      /* remember this pbuf for linkage in next iteration */
      r = q;
    }
    /* end of chain */
    /*r->next = NULL;*/

    break;
  case PBUF_RAM:
    /* If pbuf is to be allocated in RAM, allocate memory for it. */
    p = (struct pbuf*)mem_malloc(LWIP_MEM_ALIGN_SIZE(SIZEOF_STRUCT_PBUF + offset) + LWIP_MEM_ALIGN_SIZE(length));
    if (p == NULL) {
      return NULL;
    }
    /* Set up internal structure of the pbuf. */
    p->payload = LWIP_MEM_ALIGN((void *)((u8_t *)p + SIZEOF_STRUCT_PBUF + offset));
    p->len = p->tot_len = length;
    p->next = NULL;
    p->type = type;

    LWIP_ASSERT("pbuf_alloc: pbuf->payload properly aligned",
           ((mem_ptr_t)p->payload % MEM_ALIGNMENT) == 0);
    break;
  /* pbuf references existing (non-volatile static constant) ROM payload? */
  case PBUF_ROM:
  /* pbuf references existing (externally allocated) RAM payload? */
  case PBUF_REF:
    /* only allocate memory for the pbuf structure */
    p = (struct pbuf *)memp_malloc(MEMP_PBUF);
    if (p == NULL) {
      LWIP_DEBUGF(PBUF_DEBUG | LWIP_DBG_LEVEL_SERIOUS,
                  ("pbuf_alloc: Could not allocate MEMP_PBUF for PBUF_%s.\n",
                  (type == PBUF_ROM) ? "ROM" : "REF"));
      return NULL;
    }
    /* caller must set this field properly, afterwards */
    p->payload = NULL;
    p->len = p->tot_len = length;
    p->next = NULL;
    p->type = type;
    break;
  default:
    LWIP_ASSERT("pbuf_alloc: erroneous type", 0);
    return NULL;
  }
  /* set reference count */
  p->ref = 1;
  /* set flags */
  p->flags = 0;
  LWIP_DEBUGF(PBUF_DEBUG | LWIP_DBG_TRACE, ("pbuf_alloc(length=%"U16_F") == %p\n", length, (void *)p));
  return p;
}

#if LWIP_SUPPORT_CUSTOM_PBUF          /* 暂不考虑 */
/** Initialize a custom pbuf (already allocated).
 *
 * @param layer flag to define header size
 * @param length size of the pbuf's payload
 * @param type type of the pbuf (only used to treat the pbuf accordingly, as
 *        this function allocates no memory)
 * @param p pointer to the custom pbuf to initialize (already allocated)
 * @param payload_mem pointer to the buffer that is used for payload and headers,
 *        must be at least big enough to hold 'length' plus the header size,
 *        may be NULL if set later.
 *        ATTENTION: The caller is responsible for correct alignment of this buffer!!
 * @param payload_mem_len the size of the 'payload_mem' buffer, must be at least
 *        big enough to hold 'length' plus the header size
 */
struct pbuf*
pbuf_alloced_custom(pbuf_layer l, u16_t length, pbuf_type type, struct pbuf_custom *p,
                    void *payload_mem, u16_t payload_mem_len)
{
  u16_t offset;
  LWIP_DEBUGF(PBUF_DEBUG | LWIP_DBG_TRACE, ("pbuf_alloced_custom(length=%"U16_F")\n", length));

  /* determine header offset */
  switch (l) {
  case PBUF_TRANSPORT:
    /* add room for transport (often TCP) layer header */
    offset = PBUF_LINK_HLEN + PBUF_IP_HLEN + PBUF_TRANSPORT_HLEN;
    break;
  case PBUF_IP:
    /* add room for IP layer header */
    offset = PBUF_LINK_HLEN + PBUF_IP_HLEN;
    break;
  case PBUF_LINK:
    /* add room for link layer header */
    offset = PBUF_LINK_HLEN;
    break;
  case PBUF_RAW:
    offset = 0;
    break;
  default:
    LWIP_ASSERT("pbuf_alloced_custom: bad pbuf layer", 0);
    return NULL;
  }

  if (LWIP_MEM_ALIGN_SIZE(offset) + length > payload_mem_len) {
    LWIP_DEBUGF(PBUF_DEBUG | LWIP_DBG_LEVEL_WARNING, ("pbuf_alloced_custom(length=%"U16_F") buffer too short\n", length));
    return NULL;
  }

  p->pbuf.next = NULL;
  if (payload_mem != NULL) {
    p->pbuf.payload = (u8_t *)payload_mem + LWIP_MEM_ALIGN_SIZE(offset);
  } else {
    p->pbuf.payload = NULL;
  }
  p->pbuf.flags = PBUF_FLAG_IS_CUSTOM;
  p->pbuf.len = p->pbuf.tot_len = length;
  p->pbuf.type = type;
  p->pbuf.ref = 1;
  return &p->pbuf;
}
#endif /* LWIP_SUPPORT_CUSTOM_PBUF */

/**
 * Shrink a pbuf chain to a desired length.
 *
 * @param p pbuf to shrink.
 * @param new_len desired new length of pbuf chain
 *
 * Depending on the desired length, the first few pbufs in a chain might
 * be skipped and left unchanged. The new last pbuf in the chain will be
 * resized, and any remaining pbufs will be freed.
 *
 * @note If the pbuf is ROM/REF, only the ->tot_len and ->len fields are adjusted.
 * @note May not be called on a packet queue.
 *
 * @note Despite its name, pbuf_realloc cannot grow the size of a pbuf (chain).
 */
void
pbuf_realloc(struct pbuf *p, u16_t new_len)
{
  struct pbuf *q;
  u16_t rem_len; /* remaining length */
  s32_t grow;

  LWIP_ASSERT("pbuf_realloc: p != NULL", p != NULL);
  LWIP_ASSERT("pbuf_realloc: sane p->type", p->type == PBUF_POOL ||
              p->type == PBUF_ROM ||
              p->type == PBUF_RAM ||
              p->type == PBUF_REF);

  /* desired length larger than current length? */
  if (new_len >= p->tot_len) {              /* 不支持调大payload区的功能，即输入的新的大小不能大于等于pbuf的payload区的总大小 */
    /* enlarging not yet supported */
    return;
  }

  /* the pbuf chain grows by (new_len - p->tot_len) bytes
   * (which may be negative in case of shrinking) */
  grow = new_len - p->tot_len;   /* 16位无符号运算，结果赋给32位有符号数 */

  /* first, step over any pbufs that should remain in the chain */
  rem_len = new_len;
  q = p;
  /* should this pbuf be kept? */
  while (rem_len > q->len) {
    /* decrease remaining length by pbuf length */
    rem_len -= q->len;
    /* decrease total length indicator */
    LWIP_ASSERT("grow < max_u16_t", grow < 0xffff);
    q->tot_len += (u16_t)grow;
    /* proceed to next pbuf in chain */
    q = q->next;
    LWIP_ASSERT("pbuf_realloc: q != NULL", q != NULL);
  }
  /* we have now reached the new last pbuf (in q) */
  /* rem_len == desired length for pbuf q */

  /* shrink allocated memory for PBUF_RAM */
  /* (other types merely adjust their length fields */
  if ((q->type == PBUF_RAM) && (rem_len != q->len)) {
    /* reallocate and adjust the length of the pbuf that will be split */
    q = (struct pbuf *)mem_trim(q, (u16_t)((u8_t *)q->payload - (u8_t *)q) + rem_len);
    LWIP_ASSERT("mem_trim returned q == NULL", q != NULL);
  }
  /* adjust length fields for new last pbuf */
  q->len = rem_len;
  q->tot_len = q->len;

  /* any remaining pbufs in chain? */
  if (q->next != NULL) {
    /* free remaining pbufs in chain */
    pbuf_free(q->next);
  }
  /* q is last packet in chain */
  q->next = NULL;

}

/*
 * Adjusts the payload pointer to hide or reveal headers in the payload.
 *
 * Adjusts the ->payload pointer so that space for a header
 * (dis)appears in the pbuf payload.
 *
 * The ->payload, ->tot_len and ->len fields are adjusted.
 *
 * @param p pbuf to change the header size.
 * @param header_size_increment Number of bytes to increment header size which
 * increases the size of the pbuf. New space is on the front.
 * (Using a negative value decreases the header size.)
 * If hdr_size_inc is 0, this function does nothing and returns succesful.
 *
 * PBUF_ROM and PBUF_REF type buffers cannot have their sizes increased, so
 * the call will fail. A check is made that the increase in header size does
 * not move the payload pointer in front of the start of the buffer.
 * @return non-zero on failure, zero on success.
 *
 */
u8_t
pbuf_header(struct pbuf *p, s16_t header_size_increment)
{
  u16_t type;
  void *payload;
  u16_t increment_magnitude;

  LWIP_ASSERT("p != NULL", p != NULL);
  if ((header_size_increment == 0) || (p == NULL)) {                   /* 参数检查 */
    return 0;
  }
 
  if (header_size_increment < 0){                /* 如果隐藏首部空间，则从首部空间向有效数据区方向调整时，不能
                                                  * 超出有效数据区的范围，否则什么都不做，退出程序 */
    increment_magnitude = -header_size_increment;
    /* Check that we aren't going to move off the end of the pbuf */
    LWIP_ERROR("increment_magnitude <= p->len", (increment_magnitude <= p->len), return 1;);
  } else {
    increment_magnitude = header_size_increment;
#if 0
    /* Can't assert these as some callers speculatively call
         pbuf_header() to see if it's OK.  Will return 1 below instead. */
    /* Check that we've got the correct type of pbuf to work with */
    LWIP_ASSERT("p->type == PBUF_RAM || p->type == PBUF_POOL", 
                p->type == PBUF_RAM || p->type == PBUF_POOL);
    /* Check that we aren't going to move off the beginning of the pbuf */
    LWIP_ASSERT("p->payload - increment_magnitude >= p + SIZEOF_STRUCT_PBUF",
                (u8_t *)p->payload - increment_magnitude >= (u8_t *)p + SIZEOF_STRUCT_PBUF);
#endif
  }

  type = p->type;
  /* remember current payload pointer */
  payload = p->payload;

  /* pbuf types containing payloads? */
  if (type == PBUF_RAM || type == PBUF_POOL) {
    /* set new payload pointer */
    p->payload = (u8_t *)p->payload - header_size_increment;
    /* boundary check fails? */
    /* 边界检查：判断调整后的payload是否指向pbuf头范围内，是则超界，取消调整，返回失败标识，退出程序 */
    if ((u8_t *)p->payload < (u8_t *)p + SIZEOF_STRUCT_PBUF) {
      LWIP_DEBUGF( PBUF_DEBUG | LWIP_DBG_LEVEL_SERIOUS,
        ("pbuf_header: failed as %p < %p (not enough space for new header size)\n",
        (void *)p->payload, (void *)(p + 1)));
      /* restore old payload pointer */
      p->payload = payload; /* 取消调整 */
      /* bail out unsuccesfully */
      return 1;  /* 返回失败标识 */
    }
  /* pbuf types refering to external payloads? */
  } else if (type == PBUF_REF || type == PBUF_ROM) {
    /* hide a header in the payload? */
    /* 只能是从首部空间向有效数据部分方向偏移且不能超出有效数据部分范围，超出则返回失败标识，退出程序 */
    if ((header_size_increment < 0) && (increment_magnitude <= p->len)) {
      /* increase payload pointer */
      p->payload = (u8_t *)p->payload - header_size_increment;
    } else {
      /* cannot expand payload to front (yet!)
       * bail out unsuccesfully */
      return 1;
    }
  } else {
    /* Unknown type */
    LWIP_ASSERT("bad pbuf type", 0);
    return 1;
  }
  /* modify pbuf length fields */
  p->len += header_size_increment;
  p->tot_len += header_size_increment;

  LWIP_DEBUGF(PBUF_DEBUG | LWIP_DBG_TRACE, ("pbuf_header: old %p new %p (%"S16_F")\n",
    (void *)payload, (void *)p->payload, header_size_increment));

  return 0;
}

/**
 * Dereference a pbuf chain or queue and deallocate any no-longer-used
 * pbufs at the head of this chain or queue.
 *
 * Decrements the pbuf reference count. If it reaches zero, the pbuf is
 * deallocated.
 *
 * For a pbuf chain, this is repeated for each pbuf in the chain,
 * up to the first pbuf which has a non-zero reference count after
 * decrementing. So, when all reference counts are one, the whole
 * chain is free'd.
 *
 * @param p The pbuf (chain) to be dereferenced.
 *
 * @return the number of pbufs that were de-allocated
 * from the head of the chain.
 *
 * @note MUST NOT be called on a packet queue (Not verified to work yet).
 * @note the reference counter of a pbuf equals the number of pointers
 * that refer to the pbuf (or into the pbuf).
 *
 * @internal examples:
 *
 * Assuming existing chains a->b->c with the following reference
 * counts, calling pbuf_free(a) results in:
 * 
 * 1->2->3 becomes ...1->3
 * 3->3->3 becomes 2->3->3
 * 1->1->2 becomes ......1
 * 2->1->1 becomes 1->1->1
 * 1->1->1 becomes .......
 *
 */
u8_t
pbuf_free(struct pbuf *p)
{
  u16_t type;
  struct pbuf *q;
  u8_t count;

  if (p == NULL) {    /* 参数检查：输入pbuf参数为NULL ，则退出程序 */
    LWIP_ASSERT("p != NULL", p != NULL);
    /* if assertions are disabled, proceed with debug output */
    LWIP_DEBUGF(PBUF_DEBUG | LWIP_DBG_LEVEL_SERIOUS,
      ("pbuf_free(p == NULL) was called.\n"));
    return 0;
  }
  LWIP_DEBUGF(PBUF_DEBUG | LWIP_DBG_TRACE, ("pbuf_free(%p)\n", (void *)p));

  PERF_START;

  LWIP_ASSERT("pbuf_free: sane type",
    p->type == PBUF_RAM || p->type == PBUF_ROM ||
    p->type == PBUF_REF || p->type == PBUF_POOL);

  count = 0;
  /* de-allocate all consecutive pbufs from the head of the chain that
   * obtain a zero reference count after decrementing*/
  while (p != NULL) {
    u16_t ref;
    SYS_ARCH_DECL_PROTECT(old_level);
    /* Since decrementing ref cannot be guaranteed to be a single machine operation
     * we must protect it. We put the new ref into a local variable to prevent
     * further protection. */
    SYS_ARCH_PROTECT(old_level);
    /* all pbufs in a chain are referenced at least once */
    LWIP_ASSERT("pbuf_free: p->ref > 0", p->ref > 0);
    /* decrease reference count (number of pointers to pbuf) */
    ref = --(p->ref);
    SYS_ARCH_UNPROTECT(old_level);
    /* this pbuf is no longer referenced to? */
    if (ref == 0) {
      /* remember next pbuf in chain for next iteration */
      q = p->next;
      LWIP_DEBUGF( PBUF_DEBUG | LWIP_DBG_TRACE, ("pbuf_free: deallocating %p\n", (void *)p));
      type = p->type;
#if LWIP_SUPPORT_CUSTOM_PBUF          /* ASYM版不包括这一功能  */
      /* is this a custom pbuf? */
      if ((p->flags & PBUF_FLAG_IS_CUSTOM) != 0) {
        struct pbuf_custom *pc = (struct pbuf_custom*)p;
        LWIP_ASSERT("pc->custom_free_function != NULL", pc->custom_free_function != NULL);
        pc->custom_free_function(p);
      } else
#endif /* LWIP_SUPPORT_CUSTOM_PBUF */
      {
        /* is this a pbuf from the pool? */
        if (type == PBUF_POOL) {
          memp_free(MEMP_PBUF_POOL, p);
        /* is this a ROM or RAM referencing pbuf? */
        } else if (type == PBUF_ROM || type == PBUF_REF) {
          memp_free(MEMP_PBUF, p);
        /* type == PBUF_RAM */
        } else {
          mem_free(p);
        }
      }
      count++;
      /* proceed to next pbuf */
      p = q;
    /* p->ref > 0, this pbuf is still referenced to */
    /* (and so the remaining pbufs in chain as well) */
    } else {
      LWIP_DEBUGF( PBUF_DEBUG | LWIP_DBG_TRACE, ("pbuf_free: %p has ref %"U16_F", ending here.\n", (void *)p, ref));
      /* stop walking through the chain */
      p = NULL;
    }
  }
  PERF_STOP("pbuf_free");
  /* return number of de-allocated pbufs */
  return count;
}

/**
 * Count number of pbufs in a chain
 *
 * @param p first pbuf of chain
 * @return the number of pbufs in a chain
 */

u8_t
pbuf_clen(struct pbuf *p)
{
  u8_t len;

  len = 0;
  while (p != NULL) {
    ++len;
    p = p->next;
/* 按照文件开头的注释，判断一个数据包的pbuf链中最后一个pbuf的方法是：p->tot_len == p->len；*/
  }
  return len;
}

/**
 * Increment the reference count of the pbuf.
 *
 * @param p pbuf to increase reference counter of
 *
 */
void
pbuf_ref(struct pbuf *p)
{
  SYS_ARCH_DECL_PROTECT(old_level);
  /* pbuf given? */
  if (p != NULL) {
    SYS_ARCH_PROTECT(old_level);
    ++(p->ref);
    SYS_ARCH_UNPROTECT(old_level);
  }
}

/**
 * Concatenate two pbufs (each may be a pbuf chain) and take over
 * the caller's reference of the tail pbuf.
 * 
 * @note The caller MAY NOT reference the tail pbuf afterwards.
 * Use pbuf_chain() for that purpose.
 * 
 * @see pbuf_chain()
 */

void
pbuf_cat(struct pbuf *h, struct pbuf *t)
{
  struct pbuf *p;

  LWIP_ERROR("(h != NULL) && (t != NULL) (programmer violates API)",
             ((h != NULL) && (t != NULL)), return;);

  /* proceed to last pbuf of chain */
  for (p = h; p->next != NULL; p = p->next) {
    /* add total length of second chain to all totals of first chain */
    p->tot_len += t->tot_len;
  }
  /* { p is last pbuf of first h chain, p->next == NULL } */
  LWIP_ASSERT("p->tot_len == p->len (of last pbuf in chain)", p->tot_len == p->len);
  LWIP_ASSERT("p->next == NULL", p->next == NULL);
  /* add total length of second chain to last pbuf total of first chain */
  p->tot_len += t->tot_len;
  /* chain last pbuf of head (p) with first of tail (t) */
  p->next = t;
  /* p->next now references t, but the caller will drop its reference to t,
   * so netto there is no change to the reference count of t.
   */
}

/**
 * Chain two pbufs (or pbuf chains) together.
 * 
 * The caller MUST call pbuf_free(t) once it has stopped
 * using it. Use pbuf_cat() instead if you no longer use t.
 * 
 * @param h head pbuf (chain)
 * @param t tail pbuf (chain)
 * @note The pbufs MUST belong to the same packet.
 * @note MAY NOT be called on a packet queue.
 *
 * The ->tot_len fields of all pbufs of the head chain are adjusted.
 * The ->next field of the last pbuf of the head chain is adjusted.
 * The ->ref field of the first pbuf of the tail chain is adjusted.
 *
 */
void
pbuf_chain(struct pbuf *h, struct pbuf *t)
{
  pbuf_cat(h, t);
  /* t is now referenced by h */
  pbuf_ref(t);
  LWIP_DEBUGF(PBUF_DEBUG | LWIP_DBG_TRACE, ("pbuf_chain: %p references %p\n", (void *)h, (void *)t));
}

/**
 * Dechains the first pbuf from its succeeding pbufs in the chain.
 *
 * Makes p->tot_len field equal to p->len.
 * @param p pbuf to dechain
 * @return remainder of the pbuf chain, or NULL if it was de-allocated.
 * @note May not be called on a packet queue.
 */
struct pbuf *
pbuf_dechain(struct pbuf *p)
{
  struct pbuf *q;
  u8_t tail_gone = 1;  /* 标识断链完成后，指定pbuf后面的第一个pbuf是否仍然存在（即没有被删除），不存在则置1，存在则置0 */
  /* tail */
  q = p->next;
  /* pbuf has successor in chain? */
  if (q != NULL) {     /* 判断第一个pbuf之后是否还有pbuf */
    /* assert tot_len invariant: (p->tot_len == p->len + (p->next? p->next->tot_len: 0) */
    LWIP_ASSERT("p->tot_len == p->len + q->tot_len", q->tot_len == p->tot_len - p->len);
    /* enforce invariant if assertion is disabled */
    q->tot_len = p->tot_len - p->len; /* ???????????????????????为什么要执行这一步????????????????????????? */
    /* decouple pbuf from remainder */
    p->next = NULL;
    /* total length of pbuf p is its own length only */
    p->tot_len = p->len;
    /* q is no longer referenced by p, free it */
    LWIP_DEBUGF(PBUF_DEBUG | LWIP_DBG_TRACE, ("pbuf_dechain: unreferencing %p\n", (void *)q));
    tail_gone = pbuf_free(q);/* 释放第二个pbuf，解除第一个pbuf对它的引用，如果第二个pbuf还被其他节点已用，则返回的是0 */
    if (tail_gone > 0) {
      LWIP_DEBUGF(PBUF_DEBUG | LWIP_DBG_TRACE,
                  ("pbuf_dechain: deallocated %p (as it is no longer referenced)\n", (void *)q));
    }
    /* return remaining tail or NULL if deallocated */
  }
  /* assert tot_len invariant: (p->tot_len == p->len + (p->next? p->next->tot_len: 0) */
  LWIP_ASSERT("p->tot_len == p->len", p->tot_len == p->len);
  return ((tail_gone > 0) ? NULL : q);
}

/*
 *
 * Create PBUF_RAM copies of pbufs.  （代码里面怎么没有见到检查目的pbuf是不是PBUF_RAM类型的呢）
 * 将属于同一个数据包的任意类型的pbuf构成的pbuf链(可以是整个数据包，也可以是数据包从某一个pbuf到最后一个
 * pbuf构成的pbuf链)中的所有有效数据复制到PBUF_RAM类型的pbuf（或pbuf链）中。
 * 本函数不支持数据包队列中数据包pbuf链的复制：源pbuf链和目的pbuf链都不能是单链形式的数据包队列中的数据
 * 包pbuf链。
 * Used to queue packets on behalf of the lwIP stack, such as
 * ARP based queueing.
 *
 * @note You MUST explicitly use p = pbuf_take(p);
 *
 * @note Only one packet is copied, no packet queue!
 *
 * @param p_to pbuf destination of the copy
 * @param p_from pbuf source of the copy 
 *
 * @return ERR_OK if pbuf was copied
 *         ERR_ARG if one of the pbufs is NULL or p_to is not big
 *                 enough to hold p_from
 *         ERR_VAL(增加)：
 */
err_t
pbuf_copy(struct pbuf *p_to, struct pbuf *p_from)
{
	/*  offset_to和offset_from表示循环时当前源pbuf和目的pbuf有效数据部分累计复制的数据长度 
   *  len表示本次循环复制数据的长度 
   */
  u16_t offset_to=0, offset_from=0, len;


  LWIP_DEBUGF(PBUF_DEBUG | LWIP_DBG_TRACE, ("pbuf_copy(%p, %p)\n",
    (void*)p_to, (void*)p_from));
  /* 参数检查：源地址和目的地址有效，且目的pbuf链数据区的有效数据部分足够大，否则返回ERR_ARG，退出程序 */
  /* is the target big enough to hold the source? */
  LWIP_ERROR("pbuf_copy: target not big enough to hold source", ((p_to != NULL) &&
             (p_from != NULL) && (p_to->tot_len >= p_from->tot_len)), return ERR_ARG;);

  /* iterate through pbuf chain */
  do
  {
    /* copy one part of the original chain */
	                                              /* *******计算本次复制长度len******** */
                                                  /* 判断当前目的pbuf的有效数据区剩余空间是否足够当前源pbuf有效数据区剩余的数据 */
    if ((p_to->len - offset_to) >= (p_from->len - offset_from)) {
      /* complete current p_from fits into current p_to */
      len = p_from->len - offset_from;            /* 足够，则本次复制的数据长度就是当前源pbuf剩余的数据 */
    } else {
      /* current p_from does not fit into current p_to */
      len = p_to->len - offset_to;                /* 不够，则本次复制的数据长度是当前目的pbuf有效数据区剩余空间长度 */
    }
	
                                                  /* ******** 开始复制数据*********** */                                              
    MEMCPY((u8_t*)p_to->payload + offset_to, (u8_t*)p_from->payload + offset_from, len); /* opt.h中配置具体实现，默认是C函数库中的memcpy函数 */
    
	                                              /* 计算下一次复制的源地址和目的地址 */
    offset_to += len;                             /* 更新本次复制后，当前目的pbuf和当前源pbuf有效数据区累计已复制数据长度 */
    offset_from += len;
    LWIP_ASSERT("offset_to <= p_to->len", offset_to <= p_to->len);
    LWIP_ASSERT("offset_from <= p_from->len", offset_from <= p_from->len);
    /* 下面两个if至少会有一个为真 */
    if (offset_from >= p_from->len) {            /* 判断当前源pbuf的有效数据区是否全部复制完成 */
      /* on to next p_from (if any) */
      offset_from = 0;
      p_from = p_from->next;                     /* 是则准备复制下一源pbuf的有效数据部分，p_from指向下一源pbuf */
    }
    if (offset_to == p_to->len) {                /* 判断目的pbuf的有效数据区空间是否已全部用完 */
      /* on to next p_to (if any) */
      offset_to = 0;
      p_to = p_to->next;                         /* 是则准备向下一目的pbuf复制数据，p_to指向下一目的pbuf  */
      LWIP_ERROR("p_to != NULL", (p_to != NULL) || (p_from == NULL) , return ERR_ARG;);
                                                 /* 如果没有下一目的pbuf(p_to==NULL)，而源pbuf还有数据要复制
                                                  * (p_from!=NULL)，则表示目的pbuf链的有效数据区空间不够，
                                                  * 返回ERR_ARG，退出程序
                                                  */
    }
    if((p_from != NULL) && (p_from->len == p_from->tot_len)) {  /* 判断当前源pbuf或下一个源pbuf是不是一个数
    	                                                         * 据包pbuf链的最后一个pbuf */
      /* don't copy more than one packet! */                    
      LWIP_ERROR("pbuf_copy() does not allow packet queues!\n",  
                 (p_from->next == NULL), return ERR_VAL;);      /* 是，判断在最后一个pbuf后面是否还有pbuf，
                                                                 * 有则表示源pbuf链是数据包队列中的一个pbuf
                                                                 * 链，则返回ERR_VAL，退出程序 */
    }
    if((p_to != NULL) && (p_to->len == p_to->tot_len)) {        /* 判断当前目的pbuf或下一目的pbuf是不是最后
    	                                                           * 一个目的pbuf */
      /* don't copy more than one packet! */
      LWIP_ERROR("pbuf_copy() does not allow packet queues!\n",  
                  (p_to->next == NULL), return ERR_VAL;);       /* 是，判断在最后一个目的pbuf后面是否还有pbuf，
                                                                 * 有则表示目的pbuf链是数据包队列中的一个pbuf链
                                                                 * 则返回ERR_VAL，退出程序 */
    }
  } while (p_from);                              /* 继续循环，直到源pbuf指针为NULL */
  LWIP_DEBUGF(PBUF_DEBUG | LWIP_DBG_TRACE, ("pbuf_copy: end of chain reached.\n"));
  return ERR_OK;                                 /* 返回成功标识 */
}

/**
 * Copy (part of) the contents of a packet buffer
 * to an application supplied buffer.
 * 将pbuf链有效数据部分指定长度的数据复制到应用缓冲存储区，如果指定的数据长度超过了pbuf链存储的数据总长度
 * buf->tot_len，则只复制buf->tot_len长度的数据；
 * @param buf the pbuf from which to copy data  源pbuf链首地址
 * @param dataptr the application supplied buffer 目的用户存储区首地址
 * @param len length of data to copy (dataptr must be big enough). No more  用户指定复制数据长度
 * than buf->tot_len will be copied, irrespective of len
 * @param offset offset into the packet buffer from where to begin copying len bytes 在payload区偏移量
 * @return the number of bytes copied, or 0 on failure  实际复制的数据长度，失败则返回0；
 *         成功：实际复制的数据长度
 *         失败：参数错误；len为0；offset>=(buf->tot_len)；offset<(buf->tot_len)&&(offset+len)>=(buf->tot_len);
 */
u16_t
pbuf_copy_partial(struct pbuf *buf, void *dataptr, u16_t len, u16_t offset)
{
  struct pbuf *p;
  u16_t left;              /* 用户存储区累计复制数据长度 */
  u16_t buf_copy_len;
  u16_t copied_total = 0;  /* pbuf链累计复制数据长度 */
  /* 参数检查：只检查两个指针参数是否为NULL，是则返回0，退出程序 */
  LWIP_ERROR("pbuf_copy_partial: invalid buf", (buf != NULL), return 0;);
  LWIP_ERROR("pbuf_copy_partial: invalid dataptr", (dataptr != NULL), return 0;);

  left = 0;               /* 用户存储区累计复制数据长度，也用作下一次复制时用户存储区的偏移量 */

  if((buf == NULL) || (dataptr == NULL)) {             /* 指针参数检查 */ 
    return 0;
  }

  /* 循环，分两个阶段：先根据偏移量，计算复制数据的起始地址；然后继续循环复制数据；
   * offset !=0 ：表示是计算复制数据起始地址阶段；
   * offset ==0 ：表示是复制数据阶段；
   * 循环终止的条件是：  剩余复制数据长度为0（表示数据正常复制完成）；
   *                   或当前pbuf指针为NULL（在计算复制起始地址阶段终止，表示offset>=(buf->tot_len);
   *                                         在复制数据阶段终止，表示是(offset+len)>=(buf->tot_len)）
   */
  /* Note some systems use byte copy if dataptr or one of the pbuf payload pointers are unaligned. */
  for(p = buf; len != 0 && p != NULL; p = p->next) { /* 判断剩余复制数据长度是否为0或当前pbuf指针为NULL，
  	                                                  * 是，则退出程序 */
  	                                                 /* 不是，则继续循环 */
    if ((offset != 0) && (offset >= p->len)) {       /* 根据offset判断当前是循环的哪个阶段：
    	                                                * 若是计算复制起始地址：则继续判断偏移量Offset是否超出当前pbuf
    	                                                *                       的有效数据部分的范围 
    	                                                * 若是复制数据：则直接跳到else */
      /* don't copy from this buffer -> on to the next */
      offset -= p->len;                              /* offset超过当前pbuf有效数据区范围，则偏移量减去当前pbuf的有
                                                      * 效数据区长度，进入下一循环，继续计算复制起始地址 */
    } else {                                         /* 否则是复制数据阶段 */
      /* copy from this buffer. maybe only partially. */
      buf_copy_len = p->len - offset;                /* 初始化本次循环复制数据长度等于当前pbuf有效数据区剩余空间大小 */
      if (buf_copy_len > len)                        /* 判断本次循环复制数据长度是否大于剩余复制数据长度 */
          buf_copy_len = len;                        /* 大于，则本次循环复制数据长度就等于剩余复制数据长度 */
      /* copy the necessary parts of the buffer */   /* 不大于，则本次循环复制数据长度就是当前pbuf有效数据区剩余空间大小 */
      MEMCPY(&((char*)dataptr)[left], &((char*)p->payload)[offset], buf_copy_len);  /* 复制数据 */
      copied_total += buf_copy_len;                  /* 更新pbuf链累计复制数据长度 */
      left += buf_copy_len;           /* 更新用户存储区累计复制数据长度，也用作下一次复制时用户存储区的偏移量*/
      len -= buf_copy_len;            /* 更新剩余复制数据长度 */
      offset = 0;                     /* offset清零，表示是在循环的复制阶段 */
    }
  }
  return copied_total;
}

/* *
 * Copy application supplied data into a pbuf.
 * 复制应用缓冲存储区的指定长度的数据到pbuf的payload区。
 * This function can only be used to copy the equivalent of buf->tot_len data.
 *
 * @param buf pbuf to fill with data
 * @param dataptr application supplied data buffer  (dataptr是”指向常量数据的非常量指针“，所谓”常量数据“ 是指数据对
 *                通过引用调用（C语言中是通过指针来实现的）的方式访问数据的主体而言，是只读的；本函数形参中”constant“
 *                修饰符限定的是通过指针dataptr对应用缓冲存储区数据的访问权限，且限定为只读。
 * @param len length of the application supplied data buffer
 *
 * @return ERR_OK if successful, ERR_MEM if the pbuf is not big enough
 */
err_t
pbuf_take(struct pbuf *buf, const void *dataptr, u16_t len)  
{
  struct pbuf *p;
  u16_t buf_copy_len;
  u16_t total_copy_len = len;
  u16_t copied_total = 0;

  LWIP_ERROR("pbuf_take: invalid buf", (buf != NULL), return 0;);
  LWIP_ERROR("pbuf_take: invalid dataptr", (dataptr != NULL), return 0;);

  if ((buf == NULL) || (dataptr == NULL) || (buf->tot_len < len)) {  /* 参数检查 */
    return ERR_ARG;
  }

  /* Note some systems use byte copy if dataptr or one of the pbuf payload pointers are unaligned. */
  for(p = buf; total_copy_len != 0; p = p->next) {        /* 判断剩余复制数据长total_copy_len是否不等于0（剩余复制数据长度初始
  	                                                       * 值为上层指定复制长度）;不等于0，继续循环复制数据；等于0，表示数据
  	                                                       * 复制完成，退出循环 */
    LWIP_ASSERT("pbuf_take: invalid pbuf", p != NULL);
    buf_copy_len = total_copy_len;                        /* 本次循环复制数据长度初始化成剩余复制数据长度 */
    if (buf_copy_len > p->len) {                          /* 判断剩余复制数据长度是否大于当前pbuf有效数据区总大小 */
      /* this pbuf cannot hold all remaining data */
      buf_copy_len = p->len;                              /* 大于，则本次循环复制数据长度等于有效数据去总大小
                                                           * 不大于，则本次循环复制数据长度等于剩余复制数据长度 */
    }
    /* copy the necessary parts of the buffer */
    MEMCPY(p->payload, &((char*)dataptr)[copied_total], buf_copy_len);  /* 复制数据 */
    total_copy_len -= buf_copy_len;                       /* 更新剩余复制数据长度 */
    copied_total += buf_copy_len;                         /* 更新累计已复制数据长度 */
  }
  LWIP_ASSERT("did not copy all data", total_copy_len == 0 && copied_total == len);
  return ERR_OK;
}

/**
 * Creates a single pbuf out of a queue of pbufs.
 *
 * @remark: Either the source pbuf 'p' is freed by this function or the original
 *          pbuf 'p' is returned, therefore the caller has to check the result!
 *
 * @param p the source pbuf
 * @param layer pbuf_layer of the new pbuf
 *
 * @return a new, single pbuf (p->next is NULL)
 *         or the old pbuf if allocation fails
 */
struct pbuf*
pbuf_coalesce(struct pbuf *p, pbuf_layer layer)
{
  struct pbuf *q;
  err_t err;
  if (p->next == NULL) {
    return p;
  }
  q = pbuf_alloc(layer, p->tot_len, PBUF_RAM);
  if (q == NULL) {
    /* @todo: what do we do now? */
    return p;
  }
  err = pbuf_copy(q, p);
  LWIP_ASSERT("pbuf_copy failed", err == ERR_OK);
  pbuf_free(p);
  return q;
}

#if LWIP_CHECKSUM_ON_COPY
/**
 * Copies data into a single pbuf (*not* into a pbuf queue!) and updates
 * the checksum while copying
 *
 * @param p the pbuf to copy data into
 * @param start_offset offset of p->payload where to copy the data to
 * @param dataptr data to copy into the pbuf
 * @param len length of data to copy into the pbuf
 * @param chksum pointer to the checksum which is updated
 * @return ERR_OK if successful, another error if the data does not fit
 *         within the (first) pbuf (no pbuf queues!)
 */
err_t
pbuf_fill_chksum(struct pbuf *p, u16_t start_offset, const void *dataptr,
                 u16_t len, u16_t *chksum)
{
  u32_t acc;
  u16_t copy_chksum;
  char *dst_ptr;
  LWIP_ASSERT("p != NULL", p != NULL);
  LWIP_ASSERT("dataptr != NULL", dataptr != NULL);
  LWIP_ASSERT("chksum != NULL", chksum != NULL);
  LWIP_ASSERT("len != 0", len != 0);

  if ((start_offset >= p->len) || (start_offset + len > p->len)) {
    return ERR_ARG;
  }

  dst_ptr = ((char*)p->payload) + start_offset;
  copy_chksum = LWIP_CHKSUM_COPY(dst_ptr, dataptr, len);
  if ((start_offset & 1) != 0) {
    copy_chksum = SWAP_BYTES_IN_WORD(copy_chksum);
  }
  acc = *chksum;
  acc += copy_chksum;
  *chksum = FOLD_U32T(acc);
  return ERR_OK;
}
#endif /* LWIP_CHECKSUM_ON_COPY */

 /** Get one byte from the specified position in a pbuf
 * WARNING: returns zero for offset >= p->tot_len
 *
 * @param p pbuf to parse
 * @param offset offset into p of the byte to return
 * @return byte at an offset into p OR ZERO IF 'offset' >= p->tot_len
 */
u8_t
pbuf_get_at(struct pbuf* p, u16_t offset)
{
  u16_t copy_from = offset;
  struct pbuf* q = p;

  /* get the correct pbuf */
  while ((q != NULL) && (q->len <= copy_from)) {
    copy_from -= q->len;
    q = q->next;
  }
  /* return requested data if pbuf is OK */
  if ((q != NULL) && (q->len > copy_from)) {
    return ((u8_t*)q->payload)[copy_from];
  }
  return 0;
}

/** Compare pbuf contents at specified offset with memory s2, both of length n
 *
 * @param p pbuf to compare
 * @param offset offset into p at wich to start comparing
 * @param s2 buffer to compare
 * @param n length of buffer to compare
 * @return zero if equal, nonzero otherwise
 *         (0xffff if p is too short, diffoffset+1 otherwise)
 */
u16_t
pbuf_memcmp(struct pbuf* p, u16_t offset, const void* s2, u16_t n)
{
  u16_t start = offset;
  struct pbuf* q = p;

  /* get the correct pbuf */
  while ((q != NULL) && (q->len <= start)) {
    start -= q->len;
    q = q->next;
  }
  /* return requested data if pbuf is OK */
  if ((q != NULL) && (q->len > start)) {
    u16_t i;
    for(i = 0; i < n; i++) {
      u8_t a = pbuf_get_at(q, start + i);
      u8_t b = ((u8_t*)s2)[i];
      if (a != b) {
        return i+1;
      }
    }
    return 0;
  }
  return 0xffff;
}

/** Find occurrence of mem (with length mem_len) in pbuf p, starting at offset
 * start_offset.
 *
 * @param p pbuf to search, maximum length is 0xFFFE since 0xFFFF is used as
 *        return value 'not found'
 * @param mem search for the contents of this buffer
 * @param mem_len length of 'mem'
 * @param start_offset offset into p at which to start searching
 * @return 0xFFFF if substr was not found in p or the index where it was found
 */
u16_t
pbuf_memfind(struct pbuf* p, const void* mem, u16_t mem_len, u16_t start_offset)
{
  u16_t i;
  u16_t max = p->tot_len - mem_len;
  if (p->tot_len >= mem_len + start_offset) {
    for(i = start_offset; i <= max; ) {
      u16_t plus = pbuf_memcmp(p, i, mem, mem_len);
      if (plus == 0) {
        return i;
      } else {
        i += plus;
      }
    }
  }
  return 0xFFFF;
}

/** Find occurrence of substr with length substr_len in pbuf p, start at offset
 * start_offset
 * WARNING: in contrast to strstr(), this one does not stop at the first \0 in
 * the pbuf/source string!
 *
 * @param p pbuf to search, maximum length is 0xFFFE since 0xFFFF is used as
 *        return value 'not found'
 * @param substr string to search for in p, maximum length is 0xFFFE
 * @return 0xFFFF if substr was not found in p or the index where it was found
 */
u16_t
pbuf_strstr(struct pbuf* p, const char* substr)
{
  size_t substr_len;
  if ((substr == NULL) || (substr[0] == 0) || (p->tot_len == 0xFFFF)) {
    return 0xFFFF;
  }
  substr_len = strlen(substr);
  if (substr_len >= 0xFFFF) {
    return 0xFFFF;
  }
  return pbuf_memfind(p, substr, (u16_t)substr_len, 0);
}
