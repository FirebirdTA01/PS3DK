void RSX_FUNC(ResetCommandBuffer)(gcmContextData *context)
{
	u32 offset = 0x1000;			// init state offset;
	RSX_FUNC(SetJumpCommand)(context,offset);

	__sync();

	gcmControlRegister volatile *ctrl = gcmGetControlRegister();
	ctrl->put = offset;
	while(ctrl->get!=offset) usleep(30);
}

void RSX_FUNC(FlushBuffer)(gcmContextData *context)
{
	u32 offset = 0;
	gcmControlRegister volatile *ctrl = gcmGetControlRegister();
	
	__sync();
	gcmAddressToOffset(context->current,&offset);
	ctrl->put = offset;
}

void RSX_FUNC(Finish)(gcmContextData *context,u32 ref_value)
{
	RSX_FUNC(SetReferenceCommand)(context,ref_value);
	RSX_FUNC(FlushBuffer)(context);

	gcmControlRegister volatile *ctrl = gcmGetControlRegister();
	while(ctrl->ref!=ref_value) usleep(30);
}

