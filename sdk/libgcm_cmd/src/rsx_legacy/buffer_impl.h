void RSX_FUNC(ResetCommandBuffer)(gcmContextData *context)
{
	u32 offset = 0x1000;			// init state offset;
#ifdef PS3TC_PROF_AUTO
	ps3dk_prof__auto_context_reset(context);
#endif
	RSX_FUNC(SetJumpCommand)(context,offset);

	__sync();

	gcmControlRegister volatile *ctrl = gcmGetControlRegister();
	ctrl->put = offset;
	while(ctrl->get!=offset) usleep(30);

	/* The RSX now reads from the start of the default buffer; so must we.
	 * (The system resets the default FIFO context here as well.) */
	context->current = context->begin;
}

void RSX_FUNC(FlushBuffer)(gcmContextData *context)
{
	u32 offset = 0;
	gcmControlRegister volatile *ctrl = gcmGetControlRegister();

	__sync();
	gcmAddressToOffset(context->current,&offset);
#ifdef PS3TC_PROF_AUTO
	const uint32_t frontier = ps3dk_prof__auto_publish_before(context);
#endif
	ctrl->put = offset;
#ifdef PS3TC_PROF_AUTO
	__sync();
	ps3dk_prof__auto_publish_after(context,frontier);
#endif
}

void RSX_FUNC(Finish)(gcmContextData *context,u32 ref_value)
{
	RSX_FUNC(SetReferenceCommand)(context,ref_value);
	RSX_FUNC(FlushBuffer)(context);

	gcmControlRegister volatile *ctrl = gcmGetControlRegister();
	while(ctrl->ref!=ref_value) usleep(30);
}
