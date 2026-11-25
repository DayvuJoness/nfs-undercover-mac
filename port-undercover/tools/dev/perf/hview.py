import numpy as np, sys
from PIL import Image
w,h=3024,1964
a=np.fromfile(sys.argv[1],dtype=np.float16).reshape(h,w,4).astype(np.float32)[:,:,:3]
m=a.max(axis=2)
Image.fromarray((np.clip(a,0,1)*255).astype(np.uint8)).resize((1512,982)).save(sys.argv[2]+'-sdr.png')
# heat: grey picture, red where above 1 (brighter = more), yellow above 1.6
g=np.clip(a.mean(axis=2),0,1)*0.45; v=np.stack([g,g,g],axis=2)
ex=np.clip(m-1,0,None)
v[:,:,0]=np.where(ex>0, 0.4+np.clip(ex/0.6,0,0.6), v[:,:,0]); v[:,:,1]=np.where(m>1.6, 0.9, v[:,:,1])
Image.fromarray((np.clip(v,0,1)*255).astype(np.uint8)).resize((1512,982)).save(sys.argv[2]+'-heat.png')
print('max',m.max(),'>1 %.1f%%'%(100*(m>1).mean()),'>1.6 %.2f%%'%(100*(m>1.6).mean()))
