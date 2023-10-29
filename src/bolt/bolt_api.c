/*
 * Copyright FalkorDB Ltd. 2023 - present
 * Licensed under the Server Side Public License v1 (SSPLv1).
 */

#include "RG.h"
#include "ws.h"
#include "bolt.h"
#include "endian.h"
#include "bolt_api.h"
#include "../util/uuid.h"
#include "../commands/commands.h"

// handle the HELLO message
static void BoltHelloCommand
(
	bolt_client_t *client  // the client that sent the message
) {
	// The HELLO message request the connection to be authorized for use with the remote database
	// input:
	// extra::Dictionary(
	//   user_agent::String,
	//   patch_bolt::List<String>,
	//   routing::Dictionary(address::String),
	//   notifications_minimum_severity::String,
	//   notifications_disabled_categories::List<String>,
	//   bolt_agent::Dictionary(
	//     product::String,
	//     platform::String,
	//     language::String,
	//     language_details::String
	//   )
	// )
	// output:
	// SUCCESS::Dictionary(
	//   server::String,
	//   connection_id::String,
	// }

	ASSERT(client != NULL);
	ASSERT(client->state == BS_NEGOTIATION);

	bolt_client_reply_for(client, BST_HELLO, BST_SUCCESS, 1);
	bolt_reply_map(client, 2);
	bolt_reply_string(client, "server", 6);
	bolt_reply_string(client, "Neo4j/5.11.0", 12);
	bolt_reply_string(client, "connection_id", 13);
	char *uuid = UUID_New();
	bolt_reply_string(client, uuid, strlen(uuid));
	rm_free(uuid);
	bolt_client_end_message(client);
	bolt_client_finish_write(client);
}

// handle the LOGON message
static void BoltLogonCommand
(
	bolt_client_t *client  // the client that sent the message
) {
	// A LOGON message carries an authentication request
	// input:
	// auth::Dictionary(
	//   scheme::String,
	//   ...
	// )
	// when schema:
	// 	 basic: principal::String and credentials::String required
	//   bearer: credentials::String required
	// output:
	// SUCCESS

	ASSERT(client != NULL);
	ASSERT(client->state == BS_AUTHENTICATION);

	bolt_client_reply_for(client, BST_LOGON, BST_SUCCESS, 1);
	bolt_reply_map(client, 0);
	bolt_client_end_message(client);
	bolt_client_finish_write(client);
}

// read the graph name from the message buffer
static RedisModuleString *get_graph_name
(
	RedisModuleCtx *ctx,   // the redis context
	bolt_client_t *client  // the client that sent the message
) {
	ASSERT(ctx != NULL);
	ASSERT(client != NULL);

	char *graph_name_str;
	uint32_t graph_name_len;
	if(bolt_read_map_size(&client->msg_buf.read) == 0) {
		// default graph name
		graph_name_str = "falkordb";
		graph_name_len = 8;
	} else {
		buffer_index_t graph_name = bolt_read_string(&client->msg_buf.read, &graph_name_len);
		graph_name_str = buffer_index_read(&graph_name, graph_name_len);
	}
	return RedisModule_CreateString(ctx, graph_name_str, graph_name_len);
}

// write the bolt value to the buffer as string
int write_value
(
	char *buff,            // the buffer to write to
	buffer_index_t *value  // the value to write
) {
	ASSERT(buff != NULL);
	ASSERT(value != NULL);

	switch (bolt_read_type(*value))
	{
		case BVT_NULL:
			bolt_read_null(value);
			return sprintf(buff, "NULL");
		case BVT_BOOL:
			return sprintf(buff, "%s", bolt_read_bool(value) ? "true" : "false");
		case BVT_INT8:
			return sprintf(buff, "%d", bolt_read_int8(value));
		case BVT_INT16:
			return sprintf(buff, "%d", bolt_read_int16(value));
		case BVT_INT32:
			return sprintf(buff, "%d", bolt_read_int32(value));
		case BVT_INT64:
			return sprintf(buff, "%lld", bolt_read_int64(value));
		case BVT_FLOAT:
			return sprintf(buff, "%f", bolt_read_float(value));
		case BVT_STRING: {
			uint32_t len;
			buffer_index_t str = bolt_read_string(value, &len);
			char *ptr = buffer_index_read(&str, len);
			return sprintf(buff, "'%.*s'", len, ptr);
		}
		case BVT_LIST: {
			uint32_t size = bolt_read_list_size(value);
			int n = 0;
			n += sprintf(buff, "[");
			if(size > 0) {
				n += write_value(buff + n, value);
				for (int i = 1; i < size; i++) {
					n += sprintf(buff + n, ", ");
					n += write_value(buff + n, value);
				}
			}
			n += sprintf(buff + n, "]");
			return n;
		}
		case BVT_MAP: {
			uint32_t size = bolt_read_map_size(value);
			int n = 0;
			n += sprintf(buff, "{");
			if(size > 0) {
				uint32_t key_len;
				buffer_index_t key = bolt_read_string(value, &key_len);
				char *key_ptr = buffer_index_read(&key, key_len);
				n += sprintf(buff + n, "%.*s: ", key_len, key_ptr);
				n += write_value(buff + n, value);
				for (int i = 1; i < size; i++) {
					key = bolt_read_string(value, &key_len);
					key_ptr = buffer_index_read(&key, key_len);
					n += sprintf(buff + n, ", ");
					n += sprintf(buff + n, "%.*s: ", key_len, key_ptr);
					n += write_value(buff + n, value);
				}
			}
			n += sprintf(buff + n, "}");
			return n;
		}
		case BVT_STRUCTURE:
			if(bolt_read_structure_type(value) == BST_POINT2D) {
				double x = bolt_read_float(value);
				double y = bolt_read_float(value);
				return sprintf(buff, "POINT({longitude: %f, latitude: %f})", x, y);
			}
			ASSERT(false);
			return 0;
		default:
			ASSERT(false);
			return 0;
	}
}

// read the query from the message buffer
RedisModuleString *get_query
(
	RedisModuleCtx *ctx,   // the redis context
	bolt_client_t *client  // the client that sent the message
) {
	ASSERT(ctx != NULL);
	ASSERT(client != NULL);

	uint32_t query_len;
	buffer_index_t query = bolt_read_string(&client->msg_buf.read, &query_len);
	buffer_index_t query_end = query;
	char *query_ptr = buffer_index_read(&query_end, query_len);
	if(query.chunk < query_end.chunk) {
		// query is split between chunks
		char query_str[query_len];
		int n = query_len;
		while(n > BUFFER_CHUNK_SIZE) {
			query_ptr = buffer_index_read(&query, 0);
			memcpy(query_str + query_len - n, query_ptr, BUFFER_CHUNK_SIZE - query.offset);
			n -= BUFFER_CHUNK_SIZE - query.offset;
			query.chunk++;
			query.offset = 0;
		}
		query_ptr = buffer_index_read(&query, 0);
		memcpy(query_str + query_len - n, query_ptr, n);
		return RedisModule_CreateString(ctx, query_str, query_len);
	}
	uint32_t params_count = bolt_read_map_size(&client->msg_buf.read);
	if(params_count > 0) {
		char prametrize_query[4096];
		int n = sprintf(prametrize_query, "CYPHER ");
		for (int i = 0; i < params_count; i++) {
			uint32_t key_len;
			buffer_index_t key = bolt_read_string(&client->msg_buf.read, &key_len);
			char *key_ptr = buffer_index_read(&key, key_len);
			n += sprintf(prametrize_query + n, "%.*s=", key_len, key_ptr);
			n += write_value(prametrize_query + n, &client->msg_buf.read);
			n += sprintf(prametrize_query + n, " ");
		}
		n += sprintf(prametrize_query + n, "%.*s", query_len, query_ptr);
		return RedisModule_CreateString(ctx, prametrize_query, n);
	}

	return RedisModule_CreateString(ctx, query_ptr, query_len);
}

RedisModuleString *COMMAND;
RedisModuleString *BOLT;

// handle the RUN message
void BoltRunCommand
(
	bolt_client_t *client  // the client that sent the message
) {
	// The RUN message requests that a Cypher query is executed with a set of parameters and additional extra data
	// input:
	// query::String,
	// parameters::Dictionary,
	// extra::Dictionary(
	//   bookmarks::List<String>,
	//   tx_timeout::Integer,
	//   tx_metadata::Dictionary,
	//   mode::String,
	//   db:String,
	//   imp_user::String,
	//   notifications_minimum_severity::String,
	//   notifications_disabled_categories::List<String>
	// )

	ASSERT(client != NULL);

	RedisModuleCtx *ctx = client->ctx;
	RedisModuleString *args[5];
	RedisModuleString *query = get_query(ctx, client);
	RedisModuleString *graph_name = get_graph_name(ctx, client);

	args[0] = COMMAND;
	args[1] = graph_name;
	args[2] = query;
	args[3] = BOLT;
	args[4] = (RedisModuleString *)client;

	CommandDispatch(ctx, args, 5);

	RedisModule_FreeString(ctx, query);
	RedisModule_FreeString(ctx, graph_name);
}

// handle the PULL message
void BoltPullCommand
(
	bolt_client_t *client  // the client that sent the message
) {
	// The PULL message requests data from the remainder of the result stream
	// input:
	// extra::Dictionary{
	//   n::Integer,
	//   qid::Integer,
	// }

	ASSERT(client != NULL);
}

// handle the BEGIN message
void BoltBeginCommand
(
	bolt_client_t *client  // the client that sent the message
) {
	// The BEGIN message request the creation of a new Explicit Transaction
	// input:
	// extra::Dictionary(
	//   bookmarks::List<String>,
	//   tx_timeout::Integer,
	//   tx_metadata::Dictionary,
	//   mode::String,
	//   db::String,
	//   imp_user::String,
	//   notifications_minimum_severity::String,
	//   notifications_disabled_categories::List<String>
	// )

	ASSERT(client != NULL);

	bolt_client_reply_for(client, BST_BEGIN, BST_SUCCESS, 1);
	bolt_reply_map(client, 0);
	bolt_client_end_message(client);
	bolt_client_finish_write(client);
}

// handle the COMMIT message
void BoltCommitCommand
(
	bolt_client_t *client  // the client that sent the message
) {
	// The COMMIT message request that the Explicit Transaction is done

	ASSERT(client != NULL);

	bolt_client_reply_for(client, BST_COMMIT, BST_SUCCESS, 1);
	bolt_reply_map(client, 0);
	bolt_client_end_message(client);
	bolt_client_finish_write(client);
}

// handle the ROLLBACK message
void BoltRollbackCommand
(
	bolt_client_t *client
) {
	// The ROLLBACK message requests that the Explicit Transaction rolls back

	ASSERT(client != NULL);

	bolt_client_reply_for(client, BST_ROLLBACK, BST_SUCCESS, 1);
	bolt_reply_map(client, 0);
	bolt_client_end_message(client);
	bolt_client_finish_write(client);
}

// handle the ROUTE message
void BoltRouteCommand
(
	bolt_client_t *client
) {
	// TThe ROUTE instructs the server to return the current routing table
	// input:
	// routing::Dictionary,
	// bookmarks::List<String>,
	// extra::Dictionary(
	//   db::String,
	//   imp_user::String,
	// )
	// output:
	// SUCCESS::Dictionary(
	//   rt::Dictionary(
	//     ttl::Integer,
	//     db::String,
	//     servers::List<Dictionary(
	//       addresses::List<String>,
	//       role::String
	//     )>
	//   )
	// )

	ASSERT(client != NULL);

	bolt_client_reply_for(client, BST_ROUTE, BST_SUCCESS, 1);
	bolt_reply_map(client, 1);
	bolt_reply_string(client, "rt", 2);
	bolt_reply_map(client, 3);
	bolt_reply_string(client, "ttl", 3);
	bolt_reply_int(client, 1000);
	bolt_reply_string(client, "db", 2);
	bolt_reply_string(client, "falkordb", 8);
	bolt_reply_string(client, "servers", 7);
	bolt_reply_list(client, 3);
	bolt_reply_map(client, 2);
	bolt_reply_string(client, "addresses", 9);
	bolt_reply_list(client, 1);
	bolt_reply_string(client, "localhost:7687", 14);
	bolt_reply_string(client, "role", 4);
	bolt_reply_string(client, "ROUTE", 5);
	bolt_reply_map(client, 2);
	bolt_reply_string(client, "addresses", 9);
	bolt_reply_list(client, 1);
	bolt_reply_string(client, "localhost:7687", 14);
	bolt_reply_string(client, "role", 4);
	bolt_reply_string(client, "READ", 4);
	bolt_reply_map(client, 2);
	bolt_reply_string(client, "addresses", 9);
	bolt_reply_list(client, 1);
	bolt_reply_string(client, "localhost:7687", 14);
	bolt_reply_string(client, "role", 4);
	bolt_reply_string(client, "WRITE", 5);
	bolt_client_end_message(client);
	bolt_client_finish_write(client);
}

// process next message from the client
void BoltRequestHandler
(
	bolt_client_t *client // the client that sent the message
) {
	ASSERT(client != NULL);

	// if there is a message already in process or
	// not enough data to read the message
	if(client->processing || buffer_index_diff(&client->read_buf.write, &client->read_buf.read) <= 2) {
		return;
	}

	// read chunked message
	buffer_index(&client->msg_buf, &client->msg_buf.read, 0);
	buffer_index(&client->msg_buf, &client->msg_buf.write, 0);
	buffer_index_t current_read = client->read_buf.read;
	if(client->ws && buffer_index_diff(&client->ws_frame, &current_read) == 0) {
		ws_read_frame(&current_read);
		client->ws_frame = current_read;
	}
	uint16_t size = ntohs(buffer_read_uint16(&current_read));
	ASSERT(size > 0);
	while(size > 0) {
		if(buffer_index_diff(&client->read_buf.write, &current_read) < size) return;
		buffer_read(&current_read, &client->msg_buf.write, size);
		size = ntohs(buffer_read_uint16(&current_read));
	}
	client->read_buf.read = current_read;
	if(buffer_index_diff(&client->read_buf.read, &client->read_buf.write) == 0) {
		buffer_index(&client->read_buf, &client->read_buf.read, 0);
		client->read_buf.write = client->read_buf.read;
		client->ws_frame = client->read_buf.read;
	}

	client->processing = true;

	switch (bolt_read_structure_type(&client->msg_buf.read))
	{
		case BST_HELLO:
			BoltHelloCommand(client);
			break;
		case BST_LOGON:
			BoltLogonCommand(client);
			break;
		case BST_LOGOFF:
			break;
		case BST_GOODBYE:
			client->processing = false;
			break;
		case BST_RUN:
			BoltRunCommand(client);
			break;
		case BST_DISCARD:
			break;
		case BST_PULL:
			BoltPullCommand(client);
			client->processing = false;
			BoltRequestHandler(client);
			break;
		case BST_BEGIN:
			BoltBeginCommand(client);
			break;
		case BST_COMMIT:
			BoltCommitCommand(client);
			break;
		case BST_ROLLBACK:
			BoltRollbackCommand(client);
			break;
		case BST_ROUTE:
			BoltRouteCommand(client);
			break;
		default:
			break;
	}
}

// read data from socket to client buffer
void BoltReadHandler
(
	int fd,           // the socket file descriptor
	void *user_data,  // the client that sent the message
	int mask          // the event mask
) {
	ASSERT(fd != -1);
	ASSERT(user_data != NULL);

	bolt_client_t *client = (bolt_client_t*)user_data;
	if(!buffer_socket_read(&client->read_buf, client->socket)) {
		// client disconnected
		RedisModule_EventLoopDel(fd, REDISMODULE_EVENTLOOP_READABLE);
		if(client->processing) {
			client->shutdown = true;
			return;
		}
		bolt_client_free(client);
		return;
	}

	// process interrupt message
	buffer_index_t current_read = client->read_buf.read;
	while(buffer_index_diff(&client->read_buf.write, &current_read) > 0) {
		uint16_t size = ntohs(buffer_read_uint16(&current_read));
		if(buffer_index_diff(&client->read_buf.write, &current_read) < size) break;
		bolt_structure_type request_type = bolt_read_structure_type(&current_read);
		if(request_type == BST_RESET) {
			ASSERT(size == 2);
			client->reset = true;
			uint16_t res = buffer_read_uint16(&current_read);
			ASSERT(res == 0);
			char *src = client->read_buf.chunks[current_read.chunk] + current_read.offset;
			char *dst = src - size - 4;
			size = buffer_index_diff(&client->read_buf.write, &current_read);
			memmove(dst, src, size);
			current_read.offset -= 6;
			client->read_buf.write.offset -= 6;
			bolt_client_finish_write(client);
		} else {
			size = ntohs(buffer_read_uint16(&current_read));
			while(size > 0) {
				if(buffer_index_diff(&client->read_buf.write, &current_read) < size) break;
				buffer_index_read(&current_read, size);
				size = ntohs(buffer_read_uint16(&current_read));
			}
			if(size > 0) break;
		}
	}

	BoltRequestHandler(client);
}

// handle the handshake process
void BoltHandshakeHandler
(
	int fd,           // the socket file descriptor
	void *user_data,  // the client that sent the message
	int mask          // the event mask
) {
	bolt_client_t *client = (bolt_client_t *)user_data;
	if(!buffer_socket_read(&client->read_buf, client->socket)) {
		// client disconnected
		RedisModule_EventLoopDel(fd, REDISMODULE_EVENTLOOP_READABLE);
		bolt_client_free(client);
		return;
	}

	if(client->ws && ws_read_frame(&client->read_buf.read) != 20) {
		RedisModule_EventLoopDel(fd, REDISMODULE_EVENTLOOP_READABLE);
		bolt_client_free(client);
		return;
	}

	if(!bolt_check_handshake(client)) {
		buffer_index(&client->write_buf, &client->write, 0);
		buffer_index(&client->read_buf, &client->read_buf.read, 0);
		if(!ws_handshake(&client->read_buf.read, &client->write)) {
			RedisModule_EventLoopDel(fd, REDISMODULE_EVENTLOOP_READABLE);
			bolt_client_free(client);
			return;
		}
		buffer_socket_write(&client->write, client->socket);
		client->ws = true;
		buffer_index(&client->write_buf, &client->write, 0);
		buffer_index(&client->read_buf, &client->read_buf.read, 0);
		buffer_index(&client->read_buf, &client->read_buf.write, 0);
		buffer_write_uint16(&client->write_buf.write, htons(0x0000));
		return;
	}

	bolt_version_t version = bolt_read_supported_version(client);

	if(client->ws) {
		buffer_write_uint8(&client->write, 0x82);
		buffer_write_uint8(&client->write, 0x04);
	}
	buffer_write_uint8(&client->write, 0x00);
	buffer_write_uint8(&client->write, 0x00);
	buffer_write_uint8(&client->write, version.minor);
	buffer_write_uint8(&client->write, version.major);
	buffer_socket_write(&client->write, client->socket);
	buffer_index(&client->write_buf, &client->write, 0);
	buffer_index(&client->read_buf, &client->read_buf.read, 0);
	buffer_index(&client->read_buf, &client->read_buf.write, 0);

	RedisModule_EventLoopDel(fd, REDISMODULE_EVENTLOOP_READABLE);
	RedisModule_EventLoopAdd(fd, REDISMODULE_EVENTLOOP_READABLE, BoltReadHandler, client);
}

// write data from client buffer to socket
void BoltResponseHandler
(
	int fd,           // the socket file descriptor
	void *user_data,  // the client that sent the message
	int mask          // the event mask
) {
	ASSERT(fd != -1);
	ASSERT(user_data != NULL);

	bolt_client_t *client = (bolt_client_t*)user_data;

	if(client->shutdown) {
		RedisModule_EventLoopDel(fd, REDISMODULE_EVENTLOOP_READABLE);
		bolt_client_free(client);
		return;
	}

	bolt_client_send(client);
	client->processing = false;

	RedisModule_EventLoopDel(fd, REDISMODULE_EVENTLOOP_WRITABLE);

	BoltRequestHandler(client);
}

// handle new client connection
void BoltAcceptHandler
(
	int fd,           // the socket file descriptor
	void *user_data,  // the client that sent the message
	int mask          // the event mask
) {
	ASSERT(fd != -1);
	ASSERT(user_data != NULL);

	RedisModuleCtx *global_ctx = (RedisModuleCtx*)user_data;

	socket_t socket = socket_accept(fd);
	if(socket == -1) return;

	if(!socket_set_non_blocking(socket)) {
		close(socket);
		return;
	}

	bolt_client_t *client = bolt_client_new(socket, global_ctx, BoltResponseHandler);
	RedisModule_EventLoopAdd(socket, REDISMODULE_EVENTLOOP_READABLE, BoltHandshakeHandler, client);
}

// listen to bolt port 7687
// add the socket to the event loop
int BoltApi_Register
(
    RedisModuleCtx *ctx  // redis context
) {
	RedisModuleServerInfoData *data = RedisModule_GetServerInfo(ctx, "Server");
	uint64_t redis_port = RedisModule_ServerInfoGetFieldUnsigned(data, "tcp_port", NULL);
	RedisModule_FreeServerInfo(ctx, data);
    socket_t bolt = socket_bind(7687 + 6379 - redis_port);
	if(bolt == -1) {
		RedisModule_Log(ctx, "warning", "Failed to bind to port 7687");
		return REDISMODULE_ERR;
	}

	RedisModuleCtx *global_ctx = RedisModule_GetDetachedThreadSafeContext(ctx);

	if(RedisModule_EventLoopAdd(bolt, REDISMODULE_EVENTLOOP_READABLE, BoltAcceptHandler, global_ctx) == REDISMODULE_ERR) {
		RedisModule_Log(ctx, "warning", "Failed to register socket accept handler");
		return REDISMODULE_ERR;
	}
	RedisModule_Log(NULL, "notice", "bolt server initialized");

	COMMAND = RedisModule_CreateString(global_ctx, "graph.QUERY", 11);
	BOLT = RedisModule_CreateString(global_ctx, "--bolt", 6);

    return REDISMODULE_OK;
}