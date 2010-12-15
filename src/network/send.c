/*
 * Copyright (c) 2010 Kevin M. Bowling, <kevin.bowling@kev009.com>, USA
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <config.h>

#include <assert.h>
#include <string.h>
#include <stdlib.h>

#include "craftd-config.h"
#include "network.h"
#include "network-private.h"
#include "packets.h"

// Hack zlib in to test chunk sending
#include <stdio.h>
#include <zlib.h>
#include <fcntl.h>
#include <sys/stat.h>

/**
 * This internal method checks login predicates, populates the rest of the
 * Player List entry, and sends the initial packet stream to spawn the player.
 * 
 * @remarks Scope: private
 * 
 * @param player Player List player pointer
 * @param username inbound username from client login packet
 * @param ver inbound version from client login packet
 */
void
process_login(struct PL_entry *player, mcstring_t *username, uint32_t ver)
{
  // TODO: Future, async check of minecraft.net for user validity
  // TODO: Future, check against local ACL
  
  /* Check if the client version is compatible with the craftd version */
  if (ver != PROTOCOL_VERSION)
  {
    const char *dconmsg = "Client version is incompatible with this server.";
    send_kick(player, mcstring_create(strlen(dconmsg), dconmsg) );
    return;
  }
  
  /* Otherwise, finish populating their Player List entry */
  pthread_rwlock_wrlock(&player->rwlock);
  mcstring_copy(player->username, username);
  pthread_rwlock_unlock(&player->rwlock);
  
  send_loginresp(player);
  send_prechunk(player, -1, -1, true); // TODO: pull spwan position from file
  //send_chunk(player, 0, 0, 0, 16, 128, 16);  // TODO: pull spawn position
  
  for(int i = -4; i < 5; i++)
  {
    for(int j = -4; j < 5; j++)
    {
      send_prechunk(player, i,j, true);
      send_chunk(player, i*16, 0, j*16, 16,128,16);
    }
  }
  
  send_spawnpos(player, 32, 260, 32); // TODO: pull spawn position from file
  //send inv
  send_movelook(player, 0, 128.1, 128.2, 0, 0, 0, false); //TODO: pull position from file
  return;
}

/**
 * Internal method that sends a handshake response packet
 *
 * @remarks Scope: private
 *
 * @param player Player List player pointer
 * @param username mcstring of the handshake username
 */
void
process_handshake(struct PL_entry *player, mcstring_t *username)
{
  struct evbuffer *output = bufferevent_get_output(player->bev);
  struct evbuffer *tempbuf = evbuffer_new();

  /* Use a non-authenticating handshake for now 
   * XXX: just a hack to get a login.
   * XXX  This needs to be added to the worker pool for computing hash
   * from minecraft.net
   */
  
  uint8_t pid = PID_HANDSHAKE;
  mcstring_t *hashreply = mcstring_create(strlen("-"), "-");
  int16_t n_hlen = htons(hashreply->slen);
  
  evbuffer_add(tempbuf, &pid, sizeof(pid));
  evbuffer_add(tempbuf, &n_hlen, sizeof(n_hlen));
  evbuffer_add(tempbuf, hashreply->str, hashreply->slen);
  
  evbuffer_add_buffer(output, tempbuf);
  evbuffer_free(tempbuf);
  mcstring_free(hashreply);
  
  return;
}

/**
 * Process a chat message or command
 */
void
process_chat(struct PL_entry *player, mcstring_t *message)
{
  if (message->str[0] == '/')
  {
    LOG(LOG_INFO, "Command: %s", message->str);
    send_directchat(player, message);
    // process_cmd
  }
  else
  {
    send_chat(player, message);
  }
}

/**
 * Internal method that sends a login response packet
 *
 * @remarks Scope: private
 * 
 * @param player Player List player pointer
 */
void
send_loginresp(struct PL_entry *player)
{
  struct evbuffer *output = bufferevent_get_output(player->bev);
  struct evbuffer *tempbuf = evbuffer_new();
  
  uint8_t pid = PID_LOGIN;
  int32_t entityid = htonl(1); // TODO generate player entity IDs
  int16_t unused1 = htons(0); // Future server name? mcstring.
  int16_t unused2 = htons(0); // Future MOTD? mcstring.
  int64_t mapseed = htonll(0);
  int8_t dimension = 0;
  
  evbuffer_add(tempbuf, &pid, sizeof(pid));
  evbuffer_add(tempbuf, &entityid, sizeof(entityid));
  evbuffer_add(tempbuf, &unused1, sizeof(unused1));
  evbuffer_add(tempbuf, &unused2, sizeof(unused2));
  evbuffer_add(tempbuf, &mapseed, sizeof(mapseed));
  evbuffer_add(tempbuf, &dimension, sizeof(dimension));
  
  evbuffer_add_buffer(output, tempbuf);
  evbuffer_free(tempbuf);

  return;
}

void
send_directchat(struct PL_entry *player, mcstring_t *message)
{
  struct evbuffer *output = bufferevent_get_output(player->bev);
  struct evbuffer *tempbuf = evbuffer_new();

  uint8_t pid = PID_CHAT;
  int16_t mlen = htons(message->slen);

  evbuffer_add(tempbuf, &pid, sizeof(pid));
  evbuffer_add(tempbuf, &mlen, sizeof(mlen));
  evbuffer_add(tempbuf, message->str, message->slen);

  evbuffer_add_buffer(output, tempbuf);
  evbuffer_free(tempbuf);

  return;
}

void
send_chat(struct PL_entry *player, mcstring_t *message)
{
  mcstring_t *username;
  mcstring_t *newmessage;
  struct PL_entry *player_iter;

  char format[100] = "<";
  strncat(format, player->username->str, strlen(format)
          + player->username->slen);
  strncat(format, "> ", strlen(format));
  username = mcstring_create(strlen(format), format);

  newmessage = mcstring_mccat(username, message);
  LOG(LOG_INFO, "Chat: %s", newmessage->str);

  pthread_rwlock_rdlock(&PL_rwlock);
  SLIST_FOREACH(player_iter, &PL_head, PL_entries)
  {
    send_directchat(player_iter, newmessage);
  }
  pthread_rwlock_unlock(&PL_rwlock);

  mcstring_free(newmessage);
  mcstring_free(username);

  return;
}

/**
 * Send a prechunk packet to the player
 * 
 * @remarks Scope: public API method
 *
 * @param player Player List player pointer
 * @param x chunk x coordinate
 * @param z chunk z coordinate
 * @param mode unload (false) or load (true) the specified chunk
 */
void
send_prechunk(struct PL_entry *player, int32_t x, int32_t z, bool mode)
{
  struct evbuffer *output = bufferevent_get_output(player->bev);
  struct evbuffer *tempbuf = evbuffer_new();
  
  int8_t pid = PID_PRECHUNK;
  int32_t n_x = htonl(x);
  int32_t n_z = htonl(z);
  uint8_t n_mode = mode;
  
  evbuffer_add(tempbuf, &pid, sizeof(pid));
  evbuffer_add(tempbuf, &n_x, sizeof(n_x));
  evbuffer_add(tempbuf, &n_z, sizeof(n_z));
  evbuffer_add(tempbuf, &n_mode, sizeof(n_mode));
  
  evbuffer_add_buffer(output, tempbuf);
  evbuffer_free(tempbuf);
  
  return;
}

/**
 * Send the specified chunk to the player
 * 
 * @remarks Scope: public API method
 * @remarks Internally and over the network size{x,y,z} are -1 over the wire
 * 
 * @param player Player List player pointer
 * @param x global chunk x coordinate
 * @param y global chunk y coordinate
 * @param z global chunk z coordinate
 * @param sizex actual x size
 * @param sizey actual y size
 * @param sizez actual z size
 */
void
send_chunk(struct PL_entry *player, int32_t x, int16_t y, int32_t z,
	   uint8_t sizex, uint8_t sizey, uint8_t sizez)
{
  struct evbuffer *output = bufferevent_get_output(player->bev);
  struct evbuffer *tempbuf = evbuffer_new();
  int8_t pid = PID_MAPCHUNK;
  int32_t n_x = htonl(x);
  int16_t n_y = htons(y);
  int32_t n_z = htonl(z);
 
  /* Check that the chunk size is greater than zero since the protocol must
   * subtract one before sending.  If so, do it.
   */
  assert(sizex > 0 && sizey > 0 && sizez > 0);
  assert(sizex <= 128 && sizey <= 128 && sizez <= 128);
  --sizex;
  --sizey;
  --sizez;

  // Hack in zlib support for test  
  uint8_t *mapdata = (uint8_t*)Malloc(MAX_CHUNKARRAY);
  memset(mapdata, 0, MAX_CHUNKARRAY);
  for(int i=0; i<32768; i+=64)
    mapdata[i] = 0x01; // Stone
  memset(&mapdata[32768+16384], 255, 32768);

  uLongf written = MAX_CHUNKARRAY;
  Bytef *buffer = (Bytef*)Malloc(MAX_CHUNKARRAY);
  if (compress(buffer, &written, &mapdata[0], MAX_CHUNKARRAY) != Z_OK)
    assert(false);
  int32_t n_written = htonl(written);
 
  evbuffer_add(tempbuf, &pid, sizeof(pid));
  evbuffer_add(tempbuf, &n_x, sizeof(n_x));
  evbuffer_add(tempbuf, &n_y, sizeof(n_y));
  evbuffer_add(tempbuf, &n_z, sizeof(n_z));
  evbuffer_add(tempbuf, &sizex, sizeof(sizex));
  evbuffer_add(tempbuf, &sizey, sizeof(sizey));
  evbuffer_add(tempbuf, &sizez, sizeof(sizez));
  evbuffer_add(tempbuf, &n_written, sizeof(n_written));
  evbuffer_add(tempbuf, buffer, written);
  
  /*struct stat st;
  int fd = open("/home/kev009/c/009mc/myne2/blockdata.hex", O_RDONLY);
  fstat(fd, &st);
  
  n_written = htonl(st.st_size);
  evbuffer_add(tempbuf, &n_written, sizeof(n_written));
  evbuffer_add_file(tempbuf, fd, 0, st.st_size);*/
  
 
  evbuffer_add_buffer(output, tempbuf);
  evbuffer_free(tempbuf);
  
  free(mapdata);
  free(buffer);
 
  return;
}

/**
 * Send the client their spawn position.  Can also be used to later update
 * their compass bearing.
 * 
 * @param player Player List player pointer
 * @param x global chunk x coordinate
 * @param y global chunk y coordinate
 * @param z global chunk z coordinate
 */
void
send_spawnpos(struct PL_entry *player, int32_t x, int32_t y, int32_t z)
{
  struct evbuffer *output = bufferevent_get_output(player->bev);
  struct evbuffer *tempbuf = evbuffer_new();
  
  int8_t pid = PID_SPAWNPOS;
  int32_t n_x = htonl(x);
  int32_t n_y = htonl(y);
  int32_t n_z = htonl(z);
  
  evbuffer_add(tempbuf, &pid, sizeof(pid));
  evbuffer_add(tempbuf, &n_x, sizeof(n_x));
  evbuffer_add(tempbuf, &n_y, sizeof(n_y));
  evbuffer_add(tempbuf, &n_z, sizeof(n_z));
  
  evbuffer_add_buffer(output, tempbuf);
  evbuffer_free(tempbuf);
  
  return;
}

/**
 * Send a combined move+look packet to the player.
 * 
 * @remarks Scope: public API method
 * @remarks Note flip-flopped y and stance from client.  -_- Notch.
 * 
 * @param player Player List player pointer
 * @param x absolute x coordinate
 * @param stance modify player bounding box
 * @param y absolute y coordinate
 * @param z absolute z coordinate
 * @param yaw rotation on the x-axis 
 * @param pitch rotation on the y-axis 
 * @param flying on the ground or in the air (like 0x0A)
 */
void
send_movelook(struct PL_entry *player, double x, double stance, double y,
	      double z, float yaw, float pitch, bool flying)
{
  struct evbuffer *output = bufferevent_get_output(player->bev);
  struct evbuffer *tempbuf = evbuffer_new();
  
  int8_t pid = PID_PLAYERMOVELOOK;
  double n_x = Cswapd(x);
  double n_stance = Cswapd(stance);
  double n_y = Cswapd(y);
  double n_z = Cswapd(z);
  float n_yaw = Cswapf(yaw);
  float n_pitch = Cswapf(pitch);
  int8_t n_flying = flying; // Cast to int8 to ensure it is 1 byte
  
  evbuffer_add(tempbuf, &pid, sizeof(pid));
  evbuffer_add(tempbuf, &n_x, sizeof(n_x));
  evbuffer_add(tempbuf, &n_y, sizeof(n_y));
  evbuffer_add(tempbuf, &n_stance, sizeof(n_stance));
  evbuffer_add(tempbuf, &n_z, sizeof(n_z));
  evbuffer_add(tempbuf, &n_yaw, sizeof(n_yaw));
  evbuffer_add(tempbuf, &n_pitch, sizeof(n_pitch));
  evbuffer_add(tempbuf, &n_flying, sizeof(n_flying));
  
  evbuffer_add_buffer(output, tempbuf);
  evbuffer_free(tempbuf);
  
  return;
}

/**
 * Kick the specified player
 * 
 * @remarks Scope: public API method
 * 
 * @param player Player List player pointer
 * @param dconmsg Pointer to an mcstring with the kick message
 */
void
send_kick(struct PL_entry *player, mcstring_t *dconmsg)
{
  struct evbuffer *output = bufferevent_get_output(player->bev);
  struct evbuffer *tempbuf = evbuffer_new();
  
  uint8_t pid = PID_DISCONNECT;
  int16_t slen = htons(dconmsg->slen);

  evbuffer_add(tempbuf, &pid, sizeof(pid));
  evbuffer_add(tempbuf, &slen, sizeof(slen));
  evbuffer_add(tempbuf, dconmsg->str, dconmsg->slen);
  
  evbuffer_add_buffer(output, tempbuf);
  evbuffer_free(tempbuf);
  
  mcstring_free(dconmsg);
  
  /* TODO forcefully close the socket and perform manual cleanup if the client
   * doesn't voluntarily disconnect
   */
  
  return;
}