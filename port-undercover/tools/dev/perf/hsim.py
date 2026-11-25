import numpy as np, sys
from PIL import Image
# What the HDR presenter shows (mode 3), from a dumped RGBA16F back buffer: SDR picture and the lift factor.
w,h=3024,1964
a=np.fromfile(sys.argv[1],dtype=np.float16).reshape(h,w,4).astype(np.float32)
c=np.minimum(a[:,:,:3],1); c=np.maximum(c,0)
m=np.clip(a[:,:,3]-1,0,1)
dec=lambda x: np.where(x<=0.04045,x/12.92,((x+0.055)/1.055)**2.4)
lin0=dec(c); y=c.max(axis=2); d=np.maximum(y-0.88,0)
f=np.where(y>0.88,(y+41.67*d*d)/np.maximum(y,1e-5),1)
lin=dec(c*f[:,:,None]); s=float(sys.argv[3]) if len(sys.argv)>3 else 1.156
lin=np.where(lin>1,1+(lin-1)*s,lin)
out=lin0+(lin-lin0)*m[:,:,None]
boost=(out.max(axis=2)+1e-4)/(lin0.max(axis=2)+1e-4)
Image.fromarray((c*255).astype(np.uint8)).resize((1512,982)).save(sys.argv[2]+'-sdr.png')
g=c.mean(axis=2)*0.5
v=np.stack([g,g,g],axis=2); hot=np.clip((boost-1)/2,0,1)
v[:,:,0]=np.maximum(v[:,:,0],hot); v[:,:,1]=np.maximum(v[:,:,1],hot*0.6)
mm=(m<0.5)
v[mm]=v[mm]*0.5+np.array([0,0,0.5])
Image.fromarray((np.clip(v,0,1)*255).astype(np.uint8)).resize((1512,982)).save(sys.argv[2]+'-lift.png')
print('marked %.1f%%  lifted (boost>1.05) %.1f%%  max out %.2fx  mean boost of lifted %.2f'%(100*(m>0.5).mean(),100*(boost>1.05).mean(),out.max(),boost[boost>1.05].mean() if (boost>1.05).any() else 0))
