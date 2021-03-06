#include "Sporky.h"
#include "Session.h"
#include "geventloop.h"
#include "debugstuff.h"
#include <iostream>
#include "glib.h"
#include "purple.h"
#include "dlfcn.h"
#include "string.h"

#define READ_COND  (G_IO_IN | G_IO_HUP | G_IO_ERR)
#define WRITE_COND (G_IO_OUT | G_IO_HUP | G_IO_ERR | G_IO_NVAL)

static GMainLoop *loop;
static jobject mainObj;
static JNIEnv *mainEnv;
int running;
GMutex *loopMutex;

enum {
	TYPE_UNKNOWN = 0,
	TYPE_COMUFY,
	TYPE_EMAIL,
	TYPE_PHONE,
	TYPE_MSN,
	TYPE_YAHOO,
	TYPE_JABBER,
	TYPE_AIM,
	TYPE_ICQ
};

static const char *connectionErrors[] = {
	"CONNECTION_ERROR_NETWORK_ERROR",
	"CONNECTION_ERROR_INVALID_USERNAME",
	"CONNECTION_ERROR_AUTHENTICATION_FAILED",
	"CONNECTION_ERROR_AUTHENTICATION_IMPOSSIBLE",
	"CONNECTION_ERROR_NO_SSL_SUPPORT",
	"CONNECTION_ERROR_ENCRYPTION_ERROR",
	"CONNECTION_ERROR_NAME_IN_USE",
	"CONNECTION_ERROR_INVALID_SETTINGS",
	"CONNECTION_ERROR_CERT_NOT_PROVIDED",
	"CONNECTION_ERROR_CERT_UNTRUSTED",
	"CONNECTION_ERROR_CERT_EXPIRED",
	"CONNECTION_ERROR_CERT_NOT_ACTIVATED",
	"CONNECTION_ERROR_CERT_HOSTNAME_MISMATCH",
	"CONNECTION_ERROR_CERT_FINGERPRINT_MISMATCH",
	"CONNECTION_ERROR_CERT_SELF_SIGNED",
	"CONNECTION_ERROR_CERT_OTHER_ERROR",
	"CONNECTION_ERROR_OTHER_ERROR"
};

static const char *statusTypes[] = {
	"UNSET",
	"OFFLINE",
	"AVAILABLE",
	"UNAVAILABLE",
	"INVISIBLE",
	"AWAY",
	"EXTENDED_AWAY"
};

static int callJavaMethod(jobject object, const char *method, const char *signature, ...) {
	if (!mainEnv)
		return 0;
	jclass cls = mainEnv->GetObjectClass(object);
	jmethodID mid = mainEnv->GetMethodID(cls, method, signature);
	if (mid == 0)
		return 0;

	va_list va;
	va_start(va, signature);
	int ret = 1;
	if (signature[strlen(signature) - 1] == 'V')
		mainEnv->CallVoidMethodV(object, mid, va);
	else
		ret = (int) mainEnv->CallIntMethodV(object, mid, va);
	va_end(va);

	return ret;
}
static int enumToInt(jobject object) {
	jclass cls = mainEnv->GetObjectClass(object);
	jmethodID mid = mainEnv->GetMethodID(cls, "ordinal", "()I");
	return (int) mainEnv->CallIntMethod(object, mid);
}

static int getFD(JNIEnv *env, jobject sock) {
	jclass clazz;
	jfieldID fid;
	jobject impl;
	jobject fdesc;
	
	/* get the SocketImpl from the Socket */
	if (!(clazz = env->GetObjectClass(sock)) ||
		!(fid = env->GetFieldID(clazz,"impl","Ljava/net/SocketImpl;")) ||
		!(impl = env->GetObjectField(sock,fid))) return -1;
		
	/* get the FileDescriptor from the SocketImpl */
	if (!(clazz = env->GetObjectClass(impl)) ||
		!(fid = env->GetFieldID(clazz,"fd","Ljava/io/FileDescriptor;")) ||
		!(fdesc = env->GetObjectField(impl,fid))) return -1;
		
	/* get the fd from the FileDescriptor */
	if (!(clazz = env->GetObjectClass(fdesc)) ||
		!(fid = env->GetFieldID(clazz,"fd","I"))) return -1;
	
	/* return the descriptor */
	return env->GetIntField(fdesc,fid);
}

static jobject enum_new(const char *name, const char *value, int i) {
	jclass cls = mainEnv->FindClass(name);
	jmethodID mid = mainEnv->GetMethodID (cls, "<init>", "(Ljava/lang/String;I)V");
	jobject object = mainEnv->NewObject(cls, mid, mainEnv->NewStringUTF(value), i);
	return object;
}

static jobject buddy_new(PurpleBuddy *);

static void buddy_update_status(PurpleBuddy *b) {
	if (b->node.ui_data == NULL)
		buddy_new(b);
	jobject jBuddy = (jobject) b->node.ui_data;
	jclass cls = mainEnv->GetObjectClass(jBuddy);

	PurplePresence *pres = purple_buddy_get_presence(b);
	if (pres == NULL)
		return;
	PurpleStatus *stat = purple_presence_get_active_status(pres);
	if (stat == NULL)
		return;
	int status = purple_status_type_get_primitive(purple_status_get_type(stat));
	const char *message = purple_status_get_attr_string(stat, "message");

	jstring msg;
	if (message != NULL) {
		char *stripped = purple_markup_strip_html(message);
		msg = mainEnv->NewStringUTF(stripped);
		g_free(stripped);
	}
	else
		msg = mainEnv->NewStringUTF("");

	jfieldID fid;
	fid = mainEnv->GetFieldID(cls, "statusMessage", "Ljava/lang/String;");
	mainEnv->SetObjectField(jBuddy, fid, msg);

	fid = mainEnv->GetFieldID(cls, "status", "LStatusType;");
	mainEnv->SetObjectField(jBuddy, fid, enum_new("StatusType", statusTypes[status], status));
}

static void buddy_update_icon(PurpleBuddy *b) {
	if (b->node.ui_data == NULL)
		buddy_new(b);

	jobject jBuddy = (jobject) b->node.ui_data;
	jclass cls = mainEnv->GetObjectClass(jBuddy);

	gconstpointer data = NULL;
	size_t len;
	PurpleStoredImage *custom_img = NULL;
	if (b) {
		PurpleContact *contact = purple_buddy_get_contact(b);
		if (contact) {
			custom_img = purple_buddy_icons_node_find_custom_icon((PurpleBlistNode*)contact);
			if (custom_img) {
				data = purple_imgstore_get_data(custom_img);
				len = purple_imgstore_get_size(custom_img);
			}
		}
	}
	jbyteArray jb;
	if (data) {
		jb = mainEnv->NewByteArray(len);
		mainEnv->SetByteArrayRegion(jb, 0, len, (jbyte *) data);
	}
	else {
		jb = mainEnv->NewByteArray(0);
	}

	jfieldID fid;
	fid = mainEnv->GetFieldID(cls, "icon", "[B");
	mainEnv->SetObjectField(jBuddy, fid, jb);
}


static jobject buddy_new(PurpleBuddy *buddy) {
	PurpleAccount *account = purple_buddy_get_account(buddy);
	jfieldID fid;
	jclass cls = mainEnv->FindClass("Buddy");
	jmethodID mid = mainEnv->GetMethodID (cls, "<init>", "()V");
	jobject object = mainEnv->NewObject(cls, mid);

	jstring alias;
	if (purple_buddy_get_server_alias(buddy))
		alias = mainEnv->NewStringUTF(purple_buddy_get_server_alias(buddy));
	else
		alias = mainEnv->NewStringUTF(purple_buddy_get_alias(buddy));
	fid = mainEnv->GetFieldID(cls, "alias", "Ljava/lang/String;");
	mainEnv->SetObjectField(object, fid, alias);

	jstring name = mainEnv->NewStringUTF(purple_buddy_get_name(buddy));
	fid = mainEnv->GetFieldID(cls, "name", "Ljava/lang/String;");
	mainEnv->SetObjectField(object, fid, name);

	fid = mainEnv->GetFieldID(cls, "handle", "J");
	mainEnv->SetLongField(object, fid, (jlong) buddy); 

	fid = mainEnv->GetFieldID(cls, "session", "LSession;");
	mainEnv->SetObjectField(object, fid, (jobject) account->ui_data);

	buddy->node.ui_data = mainEnv->NewGlobalRef(object);

	buddy_update_status(buddy);

	return object;
}

struct BuddiesCount {
	PurpleAccount *account;
	int last_count;
};

static gboolean check_buddies_count(void *data) {
	BuddiesCount *count = (BuddiesCount *) data;
	int current_count = purple_account_get_int(count->account, "buddies_count", 0);
	std::cout << "CHECK BUDDIES COUNT " << current_count << " " << count->last_count << "\n";
	if (current_count == count->last_count) {
		GSList *buddies = purple_find_buddies(count->account, NULL);
		jobjectArray array = (jobjectArray) mainEnv->NewObjectArray(g_slist_length(buddies), mainEnv->FindClass("Buddy"), 0);
		int i = 0;
		while(buddies) {
			PurpleBuddy *b = (PurpleBuddy *) buddies->data;
			if (b->node.ui_data == NULL)
				buddy_new(b);
			buddy_update_status(b);
			jobject jBuddy = (jobject) b->node.ui_data;
			mainEnv->SetObjectArrayElement(array, i++, jBuddy);
			buddies = g_slist_delete_link(buddies, buddies);
		}
		callJavaMethod((jobject) count->account->ui_data, "onContactsReceived", "([LBuddy;)V", array);
		delete count;
		return FALSE;
	}
	count->last_count = current_count;
	return TRUE;
}

static void buddyListNewNode(PurpleBlistNode *node) {
	if (!PURPLE_BLIST_NODE_IS_BUDDY(node) || !mainEnv)
		return;
	PurpleBuddy *buddy = (PurpleBuddy *) node;
	PurpleAccount *account = purple_buddy_get_account(buddy);
	if (buddy->node.ui_data == NULL)
		buddy_new(buddy);
	callJavaMethod((jobject) account->ui_data, "onBuddyCreated", "(LBuddy;)V", (jobject) buddy->node.ui_data);

	int current_count = purple_account_get_int(account, "buddies_count", 0);
	if (current_count == 0) {
		BuddiesCount *count = new BuddiesCount;
		count->account = account;
		count->last_count = -1;
		int t = purple_timeout_add_seconds(1, &check_buddies_count, count); // TODO: remove me on disconnect
		purple_account_set_int(account, "buddies_timer", t);
	}
	purple_account_set_int(account, "buddies_count", current_count + 1);
	
}

static void buddyRemoved(PurpleBuddy *buddy, gpointer null) {
	if (buddy->node.ui_data)
		mainEnv->DeleteGlobalRef((jobject) buddy->node.ui_data);
}

static void buddyStatusChanged(PurpleBuddy *buddy, PurpleStatus *status, PurpleStatus *old_status) {
	buddy_update_status(buddy);
	callJavaMethod((jobject) buddy->node.ui_data, "onStatusChanged", "()V");
}

static void buddySignedOn(PurpleBuddy *buddy) {
	buddy_update_status(buddy);
	callJavaMethod((jobject) buddy->node.ui_data, "onSignedOn", "()V");
}

static void buddySignedOff(PurpleBuddy *buddy) {
	buddy_update_status(buddy);
	callJavaMethod((jobject) buddy->node.ui_data, "onSignedOff", "()V");
}

static void buddyIconChanged(PurpleBuddy *buddy) {
	buddy_update_icon(buddy);
	callJavaMethod((jobject) buddy->node.ui_data, "onIconChanged", "()V");
}

static void signed_on(PurpleConnection *gc,gpointer unused) {
	PurpleAccount *account = purple_connection_get_account(gc);
	callJavaMethod((jobject) account->ui_data, "onConnected", "()V");
	int current_count = purple_account_get_int(account, "buddies_count", 0);
	if (current_count == 0) {
		BuddiesCount *count = new BuddiesCount;
		count->account = account;
		count->last_count = -1;
		int t = purple_timeout_add_seconds(1, &check_buddies_count, count); // TODO: remove me on disconnect
		purple_account_set_int(account, "buddies_timer", t);
	}
}

static PurpleBlistUiOps blistUiOps =
{
	NULL,
	buddyListNewNode,
	NULL,
	NULL, // buddyListUpdate,
	NULL, //NodeRemoved,
	NULL,
	NULL,
	NULL, // buddyListAddBuddy,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL
};

static void connection_report_disconnect(PurpleConnection *gc, PurpleConnectionError reason, const char *text) {
	PurpleAccount *account = purple_connection_get_account(gc);
	jstring error = mainEnv->NewStringUTF(text ? text : "");
	callJavaMethod((jobject) account->ui_data, "onConnectionError", "(LConnectionErrorType;Ljava/lang/String;)V",
				   enum_new("ConnectionErrorType", connectionErrors[reason], reason), error);
	mainEnv->DeleteGlobalRef((jobject) account->ui_data);
	purple_timeout_remove(purple_account_get_int(account, "buddies_timer", 0));
}

static PurpleConnectionUiOps conn_ui_ops =
{
	NULL,
	NULL,
	NULL,//connection_disconnected,
	NULL,
	NULL,
	NULL,
	NULL,
	connection_report_disconnect,
	NULL,
	NULL,
	NULL
};

static void conv_write_im(PurpleConversation *conv, const char *who, const char *message, PurpleMessageFlags flags, time_t mtime) {
	if (who == NULL)
		return;

	// Don't forwards our own messages.
	if (flags & PURPLE_MESSAGE_SEND || flags & PURPLE_MESSAGE_SYSTEM)
		return;

	PurpleAccount *account = purple_conversation_get_account(conv);
	callJavaMethod((jobject) account->ui_data, "onMessageReceived", "(Ljava/lang/String;Ljava/lang/String;IJ)V",
					mainEnv->NewStringUTF(who),
					mainEnv->NewStringUTF(message),
					(int) flags,
					(jlong) mtime);
}

static PurpleConversationUiOps conversation_ui_ops =
{
	NULL,//pidgin_conv_new,
	NULL,
	NULL,                              /* write_chat           */
	conv_write_im,             /* write_im             */
	NULL,           /* write_conv           */
	NULL,       /* chat_add_users       */
	NULL,     /* chat_rename_user     */
	NULL,    /* chat_remove_users    */
	NULL,//pidgin_conv_chat_update_user,     /* chat_update_user     */
	NULL,//pidgin_conv_present_conversation, /* present              */
	NULL,//pidgin_conv_has_focus,            /* has_focus            */
	NULL,//pidgin_conv_custom_smiley_add,    /* custom_smiley_add    */
	NULL,//pidgin_conv_custom_smiley_write,  /* custom_smiley_write  */
	NULL,//pidgin_conv_custom_smiley_close,  /* custom_smiley_close  */
	NULL,//pidgin_conv_send_confirm,         /* send_confirm         */
	NULL,
	NULL,
	NULL,
	NULL
};

static void transport_core_ui_init(void)
{
	purple_blist_set_ui_ops(&blistUiOps);
// 	purple_accounts_set_ui_ops(&accountUiOps);
// 	purple_notify_set_ui_ops(&notifyUiOps);
// 	purple_request_set_ui_ops(&requestUiOps);
// 	purple_xfers_set_ui_ops(getXferUiOps());
	purple_connections_set_ui_ops(&conn_ui_ops);
	purple_conversations_set_ui_ops(&conversation_ui_ops);
// #ifndef WIN32
// 	purple_dnsquery_set_ui_ops(getDNSUiOps());
// #endif
}

static PurpleCoreUiOps coreUiOps =
{
	NULL,
	debug_init,
	transport_core_ui_init,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL
};

static jobject session_new(int type) {
	jfieldID fid;
	jclass cls = mainEnv->FindClass("Session");
	jmethodID mid = mainEnv->GetMethodID (cls, "<init>", "()V");
	jobject object = mainEnv->NewObject(cls, mid);

	fid = mainEnv->GetFieldID(cls, "type", "I");
	mainEnv->SetIntField(object, fid, type); 

	fid = mainEnv->GetFieldID(cls, "sporky", "LSporky;");
	mainEnv->SetObjectField(object, fid, (jobject) mainObj); 

	return object;
}

static void *jobject_get_handle(jobject object) {
	jclass cls = mainEnv->GetObjectClass(object);
	jfieldID fid  = mainEnv->GetFieldID(cls, "handle", "J");
	jlong handle = mainEnv->GetLongField(object, fid); 
	return (void *) handle;
}

static PurpleAccount *session_get_account(jobject object) {
	return (PurpleAccount *) jobject_get_handle(object);
}


JNIEXPORT jint JNICALL Java_Sporky_init(JNIEnv *env, jobject obj, jstring _dir) {
	dlopen("libpurple.so", RTLD_NOW|RTLD_GLOBAL);
	g_thread_init(NULL);
	loopMutex = g_mutex_new();

	const char *dir = env->GetStringUTFChars(_dir, 0);
	purple_util_set_user_dir(dir);
	env->ReleaseStringUTFChars(_dir, dir);

	purple_debug_set_ui_ops(getDebugUiOps());

	purple_core_set_ui_ops(&coreUiOps);
	purple_eventloop_set_ui_ops(getEventLoopUiOps());

	int ret = purple_core_init("sporky");
	if (ret) {
		static int conversation_handle;
		static int conn_handle;
		static int blist_handle;

		purple_set_blist(purple_blist_new());
// 		purple_blist_load();

		purple_prefs_load();
		purple_signal_connect(purple_connections_get_handle(), "signed-on", &conn_handle,PURPLE_CALLBACK(signed_on), NULL);
		purple_signal_connect(purple_blist_get_handle(), "buddy-removed", &blist_handle,PURPLE_CALLBACK(buddyRemoved), NULL);
		purple_signal_connect(purple_blist_get_handle(), "buddy-signed-on", &blist_handle,PURPLE_CALLBACK(buddySignedOn), NULL);
		purple_signal_connect(purple_blist_get_handle(), "buddy-signed-off", &blist_handle,PURPLE_CALLBACK(buddySignedOff), NULL);
		purple_signal_connect(purple_blist_get_handle(), "buddy-status-changed", &blist_handle,PURPLE_CALLBACK(buddyStatusChanged), NULL);
		purple_signal_connect(purple_blist_get_handle(), "buddy-icon-changed", &blist_handle,PURPLE_CALLBACK(buddyIconChanged), NULL);
	}
	mainObj = env->NewGlobalRef(obj);
	mainEnv = env;
	return ret;
}

struct connectData {
	std::string name;
	std::string password;
	int type;
	jobject session;
	void *ref;
};

static gboolean libpurple_connect(void *d) {
	connectData *data = (connectData *) d;
	jfieldID fid;
	static char prpl[30];
	switch(data->type) {
		case TYPE_JABBER:
			strcpy(prpl, "prpl-jabber");
			break;
		case TYPE_ICQ:
			strcpy(prpl, "prpl-icq");
			break;
		case TYPE_MSN:
			strcpy(prpl, "prpl-msn");
			break;
		case TYPE_AIM:
			strcpy(prpl, "prpl-aim");
			break;
		case TYPE_YAHOO:
			strcpy(prpl, "prpl-yahoo");
			break;
	}

	PurpleAccount *account = purple_accounts_find(data->name.c_str(), prpl);
	if (account) {
		purple_account_set_int(account, "buddies_count", 0);
		GList *iter;
		for (iter = purple_get_conversations(); iter; ) {
			PurpleConversation *conv = (PurpleConversation*) iter->data;
			iter = iter->next;
			if (purple_conversation_get_account(conv) == account)
				purple_conversation_destroy(conv);
		}
// 		purple_accounts_delete(account);
	}
	account = purple_account_new(data->name.c_str(), prpl);
	purple_account_set_password(account, data->password.c_str());
	purple_account_set_enabled(account, "sporky", TRUE);
	purple_accounts_add(account);

	jclass cls = mainEnv->GetObjectClass(data->session);
	jstring name = mainEnv->NewStringUTF(purple_account_get_username(account));
	fid = mainEnv->GetFieldID(cls, "name", "Ljava/lang/String;");
	mainEnv->SetObjectField(data->session, fid, name);

	fid = mainEnv->GetFieldID(cls, "handle", "J");
	mainEnv->SetLongField(data->session, fid, (jlong) account); 
	account->ui_data = data->ref;


	delete data;
	return FALSE;
}

JNIEXPORT jobject JNICALL Java_Sporky_connect (JNIEnv *env, jobject sporky, jstring _name, jobject _type, jstring _password) {
	connectData *data = new connectData;
	const char *name = env->GetStringUTFChars(_name, 0);
	data->name = name;
	const char *password = env->GetStringUTFChars(_password, 0);
	data->password = password;
	data->type = enumToInt(_type);
	data->session = session_new(data->type);
	data->ref = mainEnv->NewGlobalRef(data->session);
	purple_timeout_add(10, &libpurple_connect, data);

	env->ReleaseStringUTFChars(_name, name);
	env->ReleaseStringUTFChars(_password, password);
	
	return data->session;
}

JNIEXPORT void JNICALL Java_Session_disconnect (JNIEnv *env, jobject ses) {
	PurpleAccount *account = session_get_account(ses);
	mainEnv->DeleteGlobalRef((jobject) account->ui_data);
	purple_account_set_enabled(account, "sporky", FALSE);
}

static gboolean poll_timeout(void *data) {
	return TRUE;
}

JNIEXPORT void JNICALL Java_Sporky_start (JNIEnv *env, jobject obj) {
	// only first thread can start event loop, others thread will do that on next call
	if (g_mutex_trylock(loopMutex)) {
		if (!loop)
			loop = g_main_loop_new(NULL, FALSE);
		running = 1;
		while (running) {
			int ret = true;
			while (ret) {
				ret = g_main_context_iteration(g_main_loop_get_context(loop), false);
			}
			g_usleep(G_USEC_PER_SEC/10);
		}
		g_mutex_unlock(loopMutex);
	}
	else {
		// block the thread until stop() is called
		g_mutex_lock(loopMutex);
		g_mutex_unlock(loopMutex);
	}
}

JNIEXPORT void JNICALL Java_Session_setStatus (JNIEnv *env, jobject ses, jobject _type, jstring _message) {
	PurpleAccount *account = session_get_account(ses);
	int type = enumToInt(_type);
	const PurpleStatusType *status_type = purple_account_get_status_type_with_primitive(account, (PurpleStatusPrimitive) type);
	if (status_type != NULL) {
		const char *message = env->GetStringUTFChars(_message, 0);
		if (strlen(message) != 0) {
			purple_account_set_status(account, purple_status_type_get_id(status_type), TRUE, "message", message, NULL);
		}
		else {
			purple_account_set_status(account, purple_status_type_get_id(status_type), TRUE, NULL);
		}
		env->ReleaseStringUTFChars(_message, message);
	}
}

static gboolean stop_libpurple(void *data) {
	// TODO: REMOVE ME SOMEWHERE IN Sporky CLASS DESTRUCTOR
// 	purple_blist_uninit();
// 	purple_core_quit();
	
// 	if (loop) {
// 		g_main_loop_quit(loop);
// 		g_main_loop_unref(loop);
// 		loop = NULL;
// 	}
	running = 0;
	return FALSE;
}

JNIEXPORT void JNICALL Java_Sporky_stop (JNIEnv *env, jobject) {
	
	purple_timeout_add(10, &stop_libpurple, NULL);
}

JNIEXPORT void JNICALL Java_Session_sendMessage (JNIEnv *env, jobject ses, jstring _to, jstring _message) {
	const char *to = env->GetStringUTFChars(_to, 0);
	const char *message = env->GetStringUTFChars(_message, 0);
	PurpleAccount *account = session_get_account(ses);
	
	if (account) {
		PurpleConversation *conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, to, account);
		if (!conv)
			conv = purple_conversation_new(PURPLE_CONV_TYPE_IM, account, to);

		gchar *_markup = purple_markup_escape_text(message, -1);
		purple_conv_im_send(PURPLE_CONV_IM(conv), _markup);
	}
	
	env->ReleaseStringUTFChars(_to, to);
	env->ReleaseStringUTFChars(_message, message);
}

struct timerCallback {
	jobject obj;
	char *cb;
	int handle;
};

static gboolean _timer_callback(void *data) {
	timerCallback *d  = (timerCallback *) data;
	int ret = callJavaMethod(d->obj, d->cb, "()I");
	if (ret == 0) {
		mainEnv->DeleteGlobalRef(d->obj);
		g_free(d->cb);
		delete d;
	}
	return ret;
}

JNIEXPORT jint JNICALL Java_Sporky_addTimer (JNIEnv *env, jobject, jobject obj, jstring callback, jint ms) {
	int handle;
	const char *cb = env->GetStringUTFChars(callback, 0);
	timerCallback *d = new timerCallback;
	d->obj = env->NewGlobalRef(obj);
	d->cb = g_strdup(cb);
	if (ms >= 1000)
		handle = purple_timeout_add_seconds(ms / 1000, _timer_callback, d);
	else
		handle = purple_timeout_add(ms, _timer_callback, d);
	env->ReleaseStringUTFChars(callback, cb);
	return handle;
}

JNIEXPORT void JNICALL Java_Sporky_removeTimer (JNIEnv *, jobject, jint handle) {
	// TODO: remove timerCallback somehow otherwise it will leaks on both java and C++ side
	// but it's still better than potential crash.
	purple_timeout_remove(handle);
}

static void _input_callback(gpointer data, gint source, PurpleInputCondition cond) {
	timerCallback *d  = (timerCallback *) data;
	int ret = callJavaMethod(d->obj, d->cb, "(I)I");
	if (ret == 0) {
		mainEnv->DeleteGlobalRef(d->obj);
		g_free(d->cb);
		purple_input_remove(d->handle);
		delete d;
	}
}

JNIEXPORT jint JNICALL Java_Sporky_addSocketNotifier (JNIEnv *env, jobject, jobject obj, jstring callback, jobject s_obj) {
	int handle;
	int source = getFD(env, s_obj);
	if (source == -1)
		return source;
	const char *cb = env->GetStringUTFChars(callback, 0);
	timerCallback *d = new timerCallback;
	d->obj = env->NewGlobalRef(obj);
	d->cb = g_strdup(cb);
	handle = purple_input_add(source, PURPLE_INPUT_READ, _input_callback, d);
	d->handle = handle;
	env->ReleaseStringUTFChars(callback, cb);
	return handle;
}

JNIEXPORT void JNICALL Java_Sporky_removeSocketNotifier (JNIEnv *, jobject, jint handle) {
	// TODO: remove timerCallback somehow otherwise it will leaks on both java and C++ side
	// but it's still better than potential crash.
	purple_input_remove(handle);
}

JNIEXPORT void JNICALL Java_Sporky_setDebugEnabled (JNIEnv *, jobject, jint enabled) {
	purple_debug_set_enabled(enabled);
}

JNIEXPORT jobject JNICALL Java_Session_addBuddy (JNIEnv *env, jobject ses, jstring _name, jstring _alias) {
	PurpleAccount *account = session_get_account(ses);
	const char *alias = env->GetStringUTFChars(_alias, 0);
	const char *name = env->GetStringUTFChars(_name, 0);
	PurpleBuddy *buddy = purple_buddy_new(account, name, alias);
	if (buddy->node.ui_data == NULL)
		buddy_new(buddy);

	purple_blist_add_buddy(buddy, NULL, NULL ,NULL);
	purple_account_add_buddy(account, buddy);
	env->ReleaseStringUTFChars(_name, name);
	env->ReleaseStringUTFChars(_alias, alias);
	return (jobject) buddy->node.ui_data;
}

JNIEXPORT void JNICALL Java_Buddy_remove (JNIEnv *env, jobject jBuddy) {
	PurpleBuddy *buddy = (PurpleBuddy *) jobject_get_handle(jBuddy);
	PurpleAccount *account = purple_buddy_get_account(buddy);
	purple_account_remove_buddy(account, buddy, purple_buddy_get_group(buddy));
	purple_blist_remove_buddy(buddy);
}

JNIEXPORT void JNICALL Java_Session_setIcon (JNIEnv *env, jobject ses, jbyteArray icon) {
	jint len = env->GetArrayLength(icon);
	guchar *photo = (guchar *) g_malloc(len * sizeof(guchar));
	env->GetByteArrayRegion(icon, 0, len,
							(jbyte *) photo);

	purple_buddy_icons_set_account_icon(session_get_account(ses), photo, len);
}
