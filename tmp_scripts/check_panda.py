from panda import Panda
p = Panda()
print('Firmware:', p.get_version())
health = p.health()
print('Safety mode:', health.get('safety_mode'))
print('Safety param:', health.get('safety_param'))
print('--- Health ---')
for k, v in health.items():
    print(f'  {k}: {v}')