Engine.RegisterCommand('debug.command', function(args)
  local amount = args.amount; assert(args.mode == 'preview' and args.enabled == true and args.asset.id == '33333333-3333-4333-8333-333333333333' and args.asset.path == 'Assets/rin.png')
  print('lua-print', amount)
  local stepped = amount + (args.enabled and 1 or 0)
  warn('lua-warn', stepped)
  Engine.AwaitSeconds(0)
  return stepped
end)

Engine.RegisterAction('debug.typed-action', function(args, context)
  assert(args.mode == 'preview' and args.enabled == true)
  assert(context.preview == true)
  if context.scene == 'Content/UI/ActionSignalScene.pxui' then
    assert(context.node == '14141414-1414-4414-8414-141414141414' and context.signal == 'studioUi.activated')
  else
    assert(context.scene == 'typed-action.pxir')
  end
  print('typed-action-start', args.amount, args.mode)
  Engine.AwaitSeconds(0)
  Engine.SetVariable('typed_action_result', args.amount * 2)
  print('typed-action-complete', args.amount * 2)
end)
