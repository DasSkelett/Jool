#include "nat64/comm/config_proto.h"
#include <linux/slab.h>


enum error_code serialize_translate_config(struct translate_config *config,
		unsigned char **buffer_out, __u16 *buffer_len_out)
{
	unsigned char *buffer;
	__u16 struct_len, mtus_len;

	struct_len = sizeof(*config);
	mtus_len = config->mtu_plateau_count * sizeof(*config->mtu_plateaus);

	buffer = kmalloc(struct_len + mtus_len, GFP_ATOMIC);
	if (!buffer) {
		log_err(ERR_ALLOC_FAILED, "Could not allocate a serialized version of the configuration.");
		return ERR_ALLOC_FAILED;
	}

	memcpy(buffer, config, sizeof(*config));
	memcpy(buffer + sizeof(*config), config->mtu_plateaus, mtus_len);

	*buffer_out = buffer;
	*buffer_len_out = struct_len + mtus_len;
	return ERR_SUCCESS;
}

enum error_code deserialize_translate_config(void *buffer, __u16 buffer_len,
		struct translate_config *target_out)
{
	__u16 mtus_len;

	memcpy(target_out, buffer, sizeof(*target_out));

	mtus_len = target_out->mtu_plateau_count * sizeof(*target_out->mtu_plateaus);
	target_out->mtu_plateaus = kmalloc(mtus_len, GFP_ATOMIC);
	if (!target_out->mtu_plateaus) {
		log_err(ERR_ALLOC_FAILED, "Could not allocate the config's plateaus.");
		return ERR_ALLOC_FAILED;
	}
	memcpy(target_out->mtu_plateaus, buffer + sizeof(*target_out), mtus_len);

	return ERR_SUCCESS;
}
