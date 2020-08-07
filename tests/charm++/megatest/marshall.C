/*
 Parameter marshalling test program
  Orion Sky Lawlor, olawlor@acm.org, 4/6/2001
 
 Creates an array, and invokes a variety of
different marshalled entry methds.
*/
class flatStruct; class pupStruct; class parent;
#include "marshall.h"
#include <string.h>

class flatStruct {
	int x;
	char str[10];
public:
	void init(void) {
		x=1234;
		strcpy(str,"foo");
	}
	void check(void) {
		if (x!=1234 || 0!=strcmp(str,"foo"))
			CkAbort("flatStruct corrupted during marshalling\n");
	}
};
PUPbytes(flatStruct)

static int *makeArr(int tryNo,int n,int extra);
static void checkArr(int *arr,int tryNo,int n);

class parent : public PUP::able {
public:
	parent() {}
	virtual const char * getSignature() {return "parent";}
	
	//PUP::able support:
	PUPable_decl(parent);
	parent(CkMigrateMessage *) {}
	// no data fields, so no pup routine
};
class child : public parent {
public:
	child() {}
	virtual const char * getSignature() {return "childe";}
	
	//PUP::able support:
	PUPable_decl(child);
	child(CkMigrateMessage *m):parent(m) {}
	// no data fields, so no pup routine
};

class pupStruct {
	int len1;
	int *arr1;
	int len2; 
	int *arr2;
	parent *subclass;
public:
	pupStruct() {arr1=NULL;arr2=NULL; subclass=NULL;}
	~pupStruct() {delete[] arr1;delete[] arr2;delete subclass;}
	void init(parent *sub=new parent()) {
		static int lenCount=0;
		len1=lenCount*5+3;
		arr1=makeArr(17,len1,0);	
		len2=lenCount*3+7;
		arr2=makeArr(18,len2,0);
		subclass=sub;
	}
	parent *getSubclass(void) {return subclass;}
	void check(void) {
		checkArr(arr1,17,len1);
		checkArr(arr2,18,len2);
	}
	void pup(PUP::er &p) {
		p|len1;
		if (p.isUnpacking()) arr1=new int[len1];
		PUParray(p,arr1,len1);
		p|len2;
		if (p.isUnpacking()) arr2=new int[len2];
		PUParray(p,arr2,len2);
		p|subclass;
	}
};


const static double basicData[10]={
	247, 'v', -31000, 65000,
	-2000000000.0,4000000000.0,
	-2000000001.0,4000000001.0,
	12.75,4123.0e23 
};


class randGen {
	unsigned int state;
public:
	randGen(unsigned int seed) {
		state=(unsigned int)(0xffffu&(seed%1280107u));
	}
	unsigned int next(void) {
		unsigned int val=(state<<16)+state;
		state=(unsigned int)(0xffffu&(val%1280107u));
		return (unsigned int)(0xffffu&val);
	}
};

static void fillArr(int tryNo,int n,int *dest) {
	randGen g(tryNo+257*n);
	for (int i=0;i<n;i++)
		dest[i]=(int)g.next();
}

static int *makeArr(int tryNo,int n,int extra)
{
	int *ret=new int[extra+n];
	fillArr(tryNo,n,&ret[extra]);
	return ret;
}
static void checkArr(int *arr,int tryNo,int n)
{
	randGen g(tryNo+257*n);
	for (int i=0;i<n;i++) {
		int expected=(int)g.next();
		if (arr[i]!=expected)
		{
			 CkError("Array marshalling error: try %d,"
					 " index %d of %d (got 0x%08x; should be 0x%08x) \n",
					tryNo,i,n,arr[i],expected);
			 CkAbort("Marshalling error");
		}
	}
}
static CkMarshallMsg *makeMsg(int tryNo,int n) {
	size_t len=n*sizeof(int);
	CkMarshallMsg *msg=new (&len,0) CkMarshallMsg;
	fillArr(tryNo,n,(int *)msg->msgBuf);
	return msg;
}
static void checkMsg(CkMarshallMsg *msg,int tryNo,int n) {
	checkArr((int *)msg->msgBuf,tryNo,n);
	delete msg;
}



static const int numElements=20;
static CProxy_marshallElt arrayProxy;
static int marshallInitVal=0,eltInitVal=0;
static void marshallInit(void) 
{
	marshallInitVal=0x1234567;
}

static int reflector=0;
void marshall_init(void)
{
  if (marshallInitVal!=0x1234567)
	CkAbort("initnode marshallInit never got called!\n");
  if (eltInitVal!=0x54321)
	CkAbort("initnode marshallElt::eltInit never got called!\n");
  reflector++;
  for (int i=0;i<numElements;i++)
  	arrayProxy[(i+reflector)%numElements].reflectMarshall(i);
  arrayProxy[0].done();
}


void marshall_moduleinit(void){
	if (CkMyPe()==0) {
		arrayProxy=CProxy_marshallElt::ckNew(); 
		for (int i=0;i<numElements;i++) arrayProxy[i].insert();
		arrayProxy.doneInserting();
	}
}


/// Send the array element ap[i] a whole set of marshalled messages.
static void callMarshallElt(const CProxy_marshallElt &ap,int i) {
     // 1
     ap[i].basic((unsigned char)basicData[0],(char)basicData[1],
				 (short)basicData[2],(unsigned short)basicData[3],
				 (int)basicData[4],(unsigned int)basicData[5],
				 (long)basicData[6],(unsigned long)basicData[7],
				 (float)basicData[8],(double)basicData[9]);

     flatStruct f; f.init();
     ap[i].flatRef(f);			// 2
     pupStruct p; 
     p.init();
     ap[i].pupped(p);			// 3
     pupStruct p2; 
     p2.init(new child);
     ap[i].puppedSubclass(p2);		// 4
     
     parent *sub=new child;
     ap[i].PUPableReference(*sub);	// 5
     ap[i].PUPablePointer(sub);		// 6
     delete sub;

     int n=0+13*i*i;
     int *arr=makeArr(1,n,0);
     ap[i].simpleArray(n,arr);		// 7
     delete[] arr;

     int m=1+i;
     int *arr2=makeArr(2,n*m,0);
     ap[i].fancyArray(arr2,n,m);	// 8
     delete[] arr2;

     int n1=2+7*i*i,n2=4+i;
     int *a1=makeArr(3,n1,1);
     int *a2=makeArr(4,n2+n,1);
     a1[0]=n2+1; a2[0]=n1+1;
     ap[i].crazyArray(a1,a2,n);		// 9
     delete[] a1;
     delete[] a2;
     
     int j;
     msgQ_t q;
     ap[i].msgQ(0,q);			// 10
     const int nQ=2;
     for (j=0;j<nQ;j++)
     	q.enq(makeMsg(5,13+2*j));
     ap[i].msgQ(nQ,q);			// 11
     for (j=0;j<nQ;j++)
     	delete q.deq();
}

class marshallElt: public CBase_marshallElt
{
    int count;
    void next(void) {
//CkPrintf("Marshall, pe %d> element %d at %d\n",CkMyPe(),thisIndex,count);
       const int numTests=11; /* number of tests listed in callMarshallElt */
       if (++count == numTests+1)  /* all tests plus done() message */
       {//All tests passed for this element
	  count=0;
          if (thisIndex==(::numElements-1)) megatest_finish();
          else  {
	    thisProxy[thisIndex+1].done();
	  }
       }
    }
    void checkChild(parent *p) {
	if (0!=strcmp("childe",p->getSignature()))
		CkAbort("Subclass not pup'ed properly");
    }
public:
    marshallElt() { 
      count=0; 
//      CkPrintf("Created marshalling object %d\n",thisIndex);
    }
    marshallElt(CkMigrateMessage *) {}
    void done(void) {next();}
    void reflectMarshall(int forElt) {
    	callMarshallElt(thisProxy,forElt);
    }

    void basic(unsigned char b,char c,
                short i,unsigned short j,
                int k,unsigned int l,
                long m,unsigned long n,
                float f,double d)
    {
       if (b!=basicData[0] || c!=(char)basicData[1] ||
           i!=(short)basicData[2] || j!=(unsigned short)(int)basicData[3] ||
           k!=(int)basicData[4] || l!=(unsigned int)basicData[5] ||
           m!=(long)basicData[6] || n!=(unsigned long)basicData[7] ||
           f!=(float)basicData[8] || d!=(double)basicData[9]) {
		CmiPrintf("b:%c %c c:%c %c i:%d %d j:%d %d, k:%d %d l:%d %d, m:%ld %ld, n:%ld %ld, f:%f %f, d:%f %f\n", b, (unsigned char)basicData[0], c, (char)basicData[1], (short)i, (short)basicData[2], (unsigned short)j, (unsigned short)(int)basicData[3], k, (int)basicData[4], l, (unsigned int)basicData[5], m, (long)basicData[6], n, (unsigned long)basicData[7], f, (float)basicData[8], d, (double)basicData[9]);
               CkAbort("Basic data marshalling test failed\n");
	}
       next();
    }
    void flatRef(flatStruct &&s) {s.check(); next();}
    void pupped(pupStruct &&s) {s.check(); next();}
    void puppedSubclass(pupStruct &&s) {
	s.check(); 
	checkChild(s.getSubclass());
	next();
    }
    void PUPableReference(parent &p) {
    	checkChild(&p);
	next();
    }
    void PUPablePointer(parent *p) {
    	checkChild(p);
    	delete p;
	next();
    }
    void simpleArray(int n,int *arr) {checkArr(arr,1,n); next();}
    void fancyArray(int *arr2,int n,int m) {checkArr(arr2,2,n*m); next();}
    void crazyArray(int *arr1,int *arr2,int n)
    {
        checkArr(&arr1[1],3,arr2[0]-1);
        checkArr(&arr2[1],4,arr1[0]-1+n);
        next();
    }
    void msgQ(int n,msgQ_t &&q) {
    	if (n!=q.length()) CkAbort("Message queue length corrupted during marshalling");
	for (int i=0;i<n;i++)
		checkMsg(q.deq(),5,13+2*i);
	next();
    }
    static void eltInit(void) {
	eltInitVal=0x54321;
    }
};

MEGATEST_REGISTER_TEST(marshall,"olawlor",1)
#include "marshall.def.h"

